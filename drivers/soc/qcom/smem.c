// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2015, Sony Mobile Communications AB.
 * Copyright (c) 2012-2013, 2017, 2019-2020 The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/hwspinlock.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/soc/qcom/smem.h>

/*
 * The Qualcomm shared memory system is a allocate only heap structure that
 * consists of one of more memory areas that can be accessed by the processors
 * in the SoC.
 *
 * All systems contains a global heap, accessible by all processors in the SoC,
 * with a table of contents data structure (@smem_header) at the beginning of
 * the main shared memory block.
 *
 * The global header contains meta data for allocations as well as a fixed list
 * of 512 entries (@smem_global_entry) that can be initialized to reference
 * parts of the shared memory space.
 *
 *
 * In addition to this global heap a set of "private" heaps can be set up at
 * boot time with access restrictions so that only certain processor pairs can
 * access the data.
 *
 * These partitions are referenced from an optional partition table
 * (@smem_ptable), that is found 4kB from the end of the main smem region. The
 * partition table entries (@smem_ptable_entry) lists the involved processors
 * (or hosts) and their location in the main shared memory region.
 *
 * Each partition starts with a header (@smem_partition_header) that identifies
 * the partition and holds properties for the two internal memory regions. The
 * two regions are cached and non-cached memory respectively. Each region
 * contain a link list of allocation headers (@smem_private_entry) followed by
 * their data.
 *
 * Items in the non-cached region are allocated from the start of the partition
 * while items in the cached region are allocated from the end. The free area
 * is hence the region between the cached and non-cached offsets. The header of
 * cached items comes after the data.
 *
 * Version 12 (SMEM_GLOBAL_PART_VERSION) changes the item alloc/get procedure
 * for the global heap. A new global partition is created from the global heap
 * region with partition type (SMEM_GLOBAL_HOST) and the max smem item count is
 * set by the bootloader.
 *
 * To synchronize allocations in the shared memory heaps a remote spinlock must
 * be held - currently lock number 3 of the sfpb or tcsr is used for this on all
 * platforms.
 *
 */

/*
 * The version member of the smem header contains an array of versions for the
 * various software components in the SoC. We verify that the boot loader
 * version is a valid version as a sanity check.
 */
#define SMEM_MASTER_SBL_VERSION_INDEX	7
#define SMEM_GLOBAL_HEAP_VERSION	11
#define SMEM_GLOBAL_PART_VERSION	12

/*
 * The first 8 items are only to be allocated by the boot loader while
 * initializing the heap.
 */
#define SMEM_ITEM_LAST_FIXED	8

/* Highest accepted item number, for both global and private heaps */
#define SMEM_ITEM_COUNT		512

/* Processor/host identifier for the application processor */
#define SMEM_HOST_APPS		0

/* Processor/host identifier for the global partition */
#define SMEM_GLOBAL_HOST	0xfffe

/* Max number of processors/hosts in a system */
#define SMEM_HOST_COUNT		13

/* Entry range check
 * ptr >= start : Checks if ptr is greater than the start of access region
 * ptr + size >= ptr: Check for integer overflow (On 32bit system where ptr
 * and size are 32bits, ptr + size can wrap around to be a small integer)
 * ptr + size <= end: Checks if ptr+size is less than the end of access region
 */
#define IN_PARTITION_RANGE(ptr, size, start, end)		\
	(((void *)(ptr) >= (void *)(start)) &&			\
	 (((void *)(ptr) + (size)) >= (void *)(ptr)) &&	\
	 (((void *)(ptr) + (size)) <= (void *)(end)))

/**
  * struct smem_proc_comm - proc_comm communication struct (legacy)
  * @command:	current command to be executed
  * @status:	status of the currently requested command
  * @params:	parameters to the command
  */
struct smem_proc_comm {
	__le32 command;
	__le32 status;
	__le32 params[2];
};

/**
 * struct smem_global_entry - entry to reference smem items on the heap
 * @allocated:	boolean to indicate if this entry is used
 * @offset:	offset to the allocated space
 * @size:	size of the allocated space, 8 byte aligned
 * @aux_base:	base address for the memory region used by this unit, or 0 for
 *		the default region. bits 0,1 are reserved
 */
struct smem_global_entry {
	__le32 allocated;
	__le32 offset;
	__le32 size;
	__le32 aux_base; /* bits 1:0 reserved */
};
#define AUX_BASE_MASK		0xfffffffc

/**
 * struct smem_header - header found in beginning of primary smem region
 * @proc_comm:		proc_comm communication interface (legacy)
 * @version:		array of versions for the various subsystems
 * @initialized:	boolean to indicate that smem is initialized
 * @free_offset:	index of the first unallocated byte in smem
 * @available:		number of bytes available for allocation
 * @reserved:		reserved field, must be 0
 * toc:			array of references to items
 */
struct smem_header {
	struct smem_proc_comm proc_comm[4];
	__le32 version[32];
	__le32 initialized;
	__le32 free_offset;
	__le32 available;
	__le32 reserved;
	struct smem_global_entry toc[SMEM_ITEM_COUNT];
};

/**
 * struct smem_ptable_entry - one entry in the @smem_ptable list
 * @offset:	offset, within the main shared memory region, of the partition
 * @size:	size of the partition
 * @flags:	flags for the partition (currently unused)
 * @host0:	first processor/host with access to this partition
 * @host1:	second processor/host with access to this partition
 * @cacheline:	alignment for "cached" entries
 * @reserved:	reserved entries for later use
 */
struct smem_ptable_entry {
	__le32 offset;
	__le32 size;
	__le32 flags;
	__le16 host0;
	__le16 host1;
	__le32 cacheline;
	__le32 reserved[7];
};

/**
 * struct smem_ptable - partition table for the private partitions
 * @magic:	magic number, must be SMEM_PTABLE_MAGIC
 * @version:	version of the partition table
 * @num_entries: number of partitions in the table
 * @reserved:	for now reserved entries
 * @entry:	list of @smem_ptable_entry for the @num_entries partitions
 */
struct smem_ptable {
	u8 magic[4];
	__le32 version;
	__le32 num_entries;
	__le32 reserved[5];
	struct smem_ptable_entry entry[];
};

static const u8 SMEM_PTABLE_MAGIC[] = { 0x24, 0x54, 0x4f, 0x43 }; /* "$TOC" */

/**
 * struct smem_partition_header - header of the partitions
 * @magic:	magic number, must be SMEM_PART_MAGIC
 * @host0:	first processor/host with access to this partition
 * @host1:	second processor/host with access to this partition
 * @size:	size of the partition
 * @offset_free_uncached: offset to the first free byte of uncached memory in
 *		this partition
 * @offset_free_cached: offset to the first free byte of cached memory in this
 *		partition
 * @reserved:	for now reserved entries
 */
struct smem_partition_header {
	u8 magic[4];
	__le16 host0;
	__le16 host1;
	__le32 size;
	__le32 offset_free_uncached;
	__le32 offset_free_cached;
	__le32 reserved[3];
};

static const u8 SMEM_PART_MAGIC[] = { 0x24, 0x50, 0x52, 0x54 };

/**
 * struct smem_private_entry - header of each item in the private partition
 * @canary:	magic number, must be SMEM_PRIVATE_CANARY
 * @item:	identifying number of the smem item
 * @size:	size of the data, including padding bytes
 * @padding_data: number of bytes of padding of data
 * @padding_hdr: number of bytes of padding between the header and the data
 * @reserved:	for now reserved entry
 */
struct smem_private_entry {
	u16 canary; /* bytes are the same so no swapping needed */
	__le16 item;
	__le32 size; /* includes padding bytes */
	__le16 padding_data;
	__le16 padding_hdr;
	__le32 reserved;
};
#define SMEM_PRIVATE_CANARY	0xa5a5

/**
 * struct smem_info - smem region info located after the table of contents
 * @magic:	magic number, must be SMEM_INFO_MAGIC
 * @size:	size of the smem region
 * @base_addr:	base address of the smem region
 * @reserved:	for now reserved entry
 * @num_items:	highest accepted item number
 */
struct smem_info {
	u8 magic[4];
	__le32 size;
	__le32 base_addr;
	__le32 reserved;
	__le16 num_items;
};

static const u8 SMEM_INFO_MAGIC[] = { 0x53, 0x49, 0x49, 0x49 }; /* SIII */

/**
 * struct smem_region - representation of a chunk of memory used for smem
 * @aux_base:	identifier of aux_mem base
 * @virt_base:	virtual base address of memory with this aux_mem identifier
 * @size:	size of the memory region
 */
struct smem_region {
	u32 aux_base;
	void __iomem *virt_base;
	size_t size;
};

/**
 * struct qcom_smem - device data for the smem device
 * @dev:	device pointer
 * @hwlock:	reference to a hwspinlock
 * @global_partition_entry: pointer to global partition entry when in use
 * @ptable_entries: list of pointers to partitions table entry of current
 *		processor/host
 * @item_count: max accepted item number
 * @num_regions: number of @regions
 * @regions:	list of the memory regions defining the shared memory
 */
struct qcom_smem {
	struct device *dev;

	struct hwspinlock *hwlock;

	struct smem_ptable_entry *global_partition_entry;
	struct smem_ptable_entry *ptable_entries[SMEM_HOST_COUNT];
	u32 item_count;

	unsigned num_regions;
	struct smem_region regions[0];
};

/* Pointer to the one and only smem handle */
static struct qcom_smem *__smem;

/* default smem host id for apps is SMEM_HOST_APPS */
static u32 smem_host_id = SMEM_HOST_APPS;

/* Timeout (ms) for the trylock of remote spinlocks */
#define HWSPINLOCK_TIMEOUT	1000

static struct smem_partition_header *
ptable_entry_to_phdr(struct smem_ptable_entry *entry)
{
	return __smem->regions[0].virt_base + le32_to_cpu(entry->offset);
}

static struct smem_private_entry *
phdr_to_last_uncached_entry(struct smem_partition_header *phdr)
{
	void *p = phdr;

	return p + le32_to_cpu(phdr->offset_free_uncached);
}

static void *phdr_to_first_cached_entry(struct smem_partition_header *phdr,
					size_t cacheline)
{
	void *p = phdr;

	return p + le32_to_cpu(phdr->size) - ALIGN(sizeof(*phdr), cacheline);
}

static void *phdr_to_last_cached_entry(struct smem_partition_header *phdr)
{
	void *p = phdr;

	return p + le32_to_cpu(phdr->offset_free_cached);
}

static struct smem_private_entry *
phdr_to_first_uncached_entry(struct smem_partition_header *phdr)
{
	void *p = phdr;

	return p + sizeof(*phdr);
}

static struct smem_private_entry *
uncached_entry_next(struct smem_private_entry *e)
{
	void *p = e;

	return p + sizeof(*e) + le16_to_cpu(e->padding_hdr) +
	       le32_to_cpu(e->size);
}

static struct smem_private_entry *
cached_entry_next(struct smem_private_entry *e, size_t cacheline)
{
	void *p = e;

	return p - le32_to_cpu(e->size) - ALIGN(sizeof(*e), cacheline);
}

static void *uncached_entry_to_item(struct smem_private_entry *e)
{
	void *p = e;

	return p + sizeof(*e) + le16_to_cpu(e->padding_hdr);
}

static void *cached_entry_to_item(struct smem_private_entry *e)
{
	void *p = e;

	return p - le32_to_cpu(e->size);
}

static int qcom_smem_alloc_private(struct qcom_smem *smem,
				   struct smem_ptable_entry *entry,
				   unsigned item,
				   size_t size)
{
	struct smem_private_entry *hdr, *end;
	struct smem_partition_header *phdr;
	struct smem_private_entry *next_hdr;
	size_t alloc_size;
	void *cached;
	void *p_end;

	phdr = ptable_entry_to_phdr(entry);
	p_end = (void *)phdr + le32_to_cpu(entry->size);

	hdr = phdr_to_first_uncached_entry(phdr);
	end = phdr_to_last_uncached_entry(phdr);
	cached = phdr_to_last_cached_entry(phdr);

	if (WARN_ON(!IN_PARTITION_RANGE(end, 0, phdr, cached) ||
						cached > p_end))
		return -EINVAL;

	while ((hdr < end) && ((hdr + 1) < end)) {
		if (hdr->canary != SMEM_PRIVATE_CANARY) {
			dev_err(smem->dev,
				"Found invalid canary in hosts %d:%d partition\n",
				phdr->host0, phdr->host1);
			return -EINVAL;
		}

		if (le16_to_cpu(hdr->item) == item)
			return -EEXIST;

		next_hdr = uncached_entry_next(hdr);

		if (WARN_ON(next_hdr <= hdr))
			return -EINVAL;

		hdr = next_hdr;
	}

	if (WARN_ON((void *)hdr > (void *)end))
		return -EINVAL;

	/* Check that we don't grow into the cached region */
	alloc_size = sizeof(*hdr) + ALIGN(size, 8);
	if ((void *)hdr + alloc_size >= cached) {
		dev_err(smem->dev, "Out of memory\n");
		return -ENOSPC;
	}

	hdr->canary = SMEM_PRIVATE_CANARY;
	hdr->item = cpu_to_le16(item);
	hdr->size = cpu_to_le32(ALIGN(size, 8));
	hdr->padding_data = cpu_to_le16(le32_to_cpu(hdr->size) - size);
	hdr->padding_hdr = 0;

	/*
	 * Ensure the header is written before we advance the free offset, so
	 * that remote processors that does not take the remote spinlock still
	 * gets a consistent view of the linked list.
	 */
	wmb();
	le32_add_cpu(&phdr->offset_free_uncached, alloc_size);

	return 0;
}

static int qcom_smem_alloc_global(struct qcom_smem *smem,
				  unsigned item,
				  size_t size)
{
	struct smem_global_entry *entry;
	struct smem_header *header;

	header = smem->regions[0].virt_base;
	entry = &header->toc[item];
	if (entry->allocated)
		return -EEXIST;

	size = ALIGN(size, 8);
	if (WARN_ON(size > le32_to_cpu(header->available)))
		return -ENOMEM;

	entry->offset = header->free_offset;
	entry->size = cpu_to_le32(size);

	/*
	 * Ensure the header is consistent before we mark the item allocated,
	 * so that remote processors will get a consistent view of the item
	 * even though they do not take the spinlock on read.
	 */
	wmb();
	entry->allocated = cpu_to_le32(1);

	le32_add_cpu(&header->free_offset, size);
	le32_add_cpu(&header->available, -size);

	return 0;
}

/**
 * qcom_smem_alloc() - allocate space for a smem item
 * @host:	remote processor id, or -1
 * @item:	smem item handle
 * @size:	number of bytes to be allocated
 *
 * Allocate space for a given smem item of size @size, given that the item is
 * not yet allocated.
 */
int qcom_smem_alloc(unsigned host, unsigned item, size_t size)
{
	struct smem_ptable_entry *entry;
	unsigned long flags;
	int ret;

	if (!__smem)
		return -EPROBE_DEFER;

	if (item < SMEM_ITEM_LAST_FIXED) {
		dev_err(__smem->dev,
			"Rejecting allocation of static entry %d\n", item);
		return -EINVAL;
	}

	if (WARN_ON(item >= __smem->item_count))
		return -EINVAL;

	ret = hwspin_lock_timeout_irqsave(__smem->hwlock,
					  HWSPINLOCK_TIMEOUT,
					  &flags);
	if (ret)
		return ret;

	if (host < SMEM_HOST_COUNT && __smem->ptable_entries[host]) {
		entry = __smem->ptable_entries[host];
		ret = qcom_smem_alloc_private(__smem, entry, item, size);
	} else if (__smem->global_partition_entry) {
		entry = __smem->global_partition_entry;
		ret = qcom_smem_alloc_private(__smem, entry, item, size);
	} else {
		ret = qcom_smem_alloc_global(__smem, item, size);
	}

	hwspin_unlock_irqrestore(__smem->hwlock, &flags);

	return ret;
}
EXPORT_SYMBOL(qcom_smem_alloc);

static void *qcom_smem_get_global(struct qcom_smem *smem,
				  unsigned item,
				  size_t *size)
{
	struct smem_global_entry *entry;
	struct smem_header *header;
	struct smem_region *area;
	u64 entry_offset;
	u32 e_size;
	u32 aux_base;
	unsigned i;

	header = smem->regions[0].virt_base;
	entry = &header->toc[item];
	if (!entry->allocated)
		return ERR_PTR(-ENXIO);

	aux_base = le32_to_cpu(entry->aux_base) & AUX_BASE_MASK;

	for (i = 0; i < smem->num_regions; i++) {
		area = &smem->regions[i];

		if (area->aux_base == aux_base || !aux_base) {
			e_size = le32_to_cpu(entry->size);
			entry_offset = le32_to_cpu(entry->offset);

			if (WARN_ON(e_size + entry_offset > area->size))
				return ERR_PTR(-EINVAL);

			if (size != NULL)
				*size = e_size;

			return area->virt_base + entry_offset;
		}
	}

	return ERR_PTR(-ENOENT);
}

static void *qcom_smem_get_private(struct qcom_smem *smem,
				   struct smem_ptable_entry *entry,
				   unsigned item,
				   size_t *size)
{
	struct smem_private_entry *e, *uncached_end, *cached_end;
	struct smem_private_entry *next_e;
	struct smem_partition_header *phdr;
	void *item_ptr, *p_end;
	size_t entry_size = 0;
	u32 partition_size;
	size_t cacheline;
	u32 padding_data;
	u32 e_size;

	phdr = ptable_entry_to_phdr(entry);
	partition_size = le32_to_cpu(entry->size);
	p_end = (void *)phdr + partition_size;
	cacheline = le32_to_cpu(entry->cacheline);

	e = phdr_to_first_uncached_entry(phdr);
	uncached_end = phdr_to_last_uncached_entry(phdr);
	cached_end = phdr_to_last_cached_entry(phdr);

	if (WARN_ON(!IN_PARTITION_RANGE(uncached_end, 0, phdr, cached_end)
					|| (void *)cached_end > p_end))
		return ERR_PTR(-EINVAL);

	while ((e < uncached_end) && ((e + 1) < uncached_end)) {
		if (e->canary != SMEM_PRIVATE_CANARY)
			goto invalid_canary;

		if (le16_to_cpu(e->item) == item) {
			e_size = le32_to_cpu(e->size);
			padding_data = le16_to_cpu(e->padding_data);

			if (e_size < partition_size && padding_data < e_size)
				entry_size = e_size - padding_data;
			else
				return ERR_PTR(-EINVAL);

			item_ptr =  uncached_entry_to_item(e);

			if (WARN_ON(!IN_PARTITION_RANGE(item_ptr, entry_size, e,
								uncached_end)))
				return ERR_PTR(-EINVAL);

			if (size != NULL)
				*size = entry_size;

			return item_ptr;
		}

		next_e = uncached_entry_next(e);
		if (WARN_ON(next_e <= e))
			return ERR_PTR(-EINVAL);

		e = next_e;
	}
	if (WARN_ON((void *)e > (void *)uncached_end))
		return ERR_PTR(-EINVAL);

	/* Item was not found in the uncached list, search the cached list */

	if (cached_end == p_end)
		return ERR_PTR(-ENOENT);

	e = phdr_to_first_cached_entry(phdr, cacheline);

	if (WARN_ON(!IN_PARTITION_RANGE(cached_end, 0, uncached_end, p_end) ||
			!IN_PARTITION_RANGE(e, sizeof(*e), cached_end, p_end)))
		return ERR_PTR(-EINVAL);

	while (e > cached_end) {
		if (e->canary != SMEM_PRIVATE_CANARY)
			goto invalid_canary;

		if (le16_to_cpu(e->item) == item) {
			e_size = le32_to_cpu(e->size);
			padding_data = le16_to_cpu(e->padding_data);

			if (e_size < partition_size && padding_data < e_size)
				entry_size  = e_size - padding_data;
			else
				return ERR_PTR(-EINVAL);

			item_ptr =  cached_entry_to_item(e);
			if (WARN_ON(!IN_PARTITION_RANGE(item_ptr, entry_size,
							cached_end, e)))
				return ERR_PTR(-EINVAL);

			if (size != NULL)
				*size = entry_size;

			return item_ptr;
		}

		next_e = cached_entry_next(e, cacheline);
		if (WARN_ON(next_e >= e))
			return ERR_PTR(-EINVAL);

		e = next_e;
	}

	if (WARN_ON((void *)e < (void *)phdr))
		return ERR_PTR(-EINVAL);

	return ERR_PTR(-ENOENT);

invalid_canary:
	dev_err(smem->dev, "Found invalid canary in hosts %d:%d partition\n",
			phdr->host0, phdr->host1);

	return ERR_PTR(-EINVAL);
}

/**
 * qcom_smem_get() - resolve ptr of size of a smem item
 * @host:	the remote processor, or -1
 * @item:	smem item handle
 * @size:	pointer to be filled out with size of the item
 *
 * Looks up smem item and returns pointer to it. Size of smem
 * item is returned in @size.
 */
void *qcom_smem_get(unsigned host, unsigned item, size_t *size)
{
	struct smem_ptable_entry *entry;
	unsigned long flags;
	int ret;
	void *ptr = ERR_PTR(-EPROBE_DEFER);

	if (!__smem)
		return ptr;

	if (WARN_ON(item >= __smem->item_count))
		return ERR_PTR(-EINVAL);

	ret = hwspin_lock_timeout_irqsave(__smem->hwlock,
					  HWSPINLOCK_TIMEOUT,
					  &flags);
	if (ret)
		return ERR_PTR(ret);

	if (host < SMEM_HOST_COUNT && __smem->ptable_entries[host]) {
		entry = __smem->ptable_entries[host];
		ptr = qcom_smem_get_private(__smem, entry, item, size);
	} else if (__smem->global_partition_entry) {
		entry = __smem->global_partition_entry;
		ptr = qcom_smem_get_private(__smem, entry, item, size);
	} else {
		ptr = qcom_smem_get_global(__smem, item, size);
	}

	hwspin_unlock_irqrestore(__smem->hwlock, &flags);

	return ptr;

}
EXPORT_SYMBOL(qcom_smem_get);

/**
 * qcom_smem_get_free_space() - retrieve amount of free space in a partition
 * @host:	the remote processor identifying a partition, or -1
 *
 * To be used by smem clients as a quick way to determine if any new
 * allocations has been made.
 */
int qcom_smem_get_free_space(unsigned host)
{
	struct smem_partition_header *phdr;
	struct smem_ptable_entry *entry;
	struct smem_header *header;
	unsigned ret;

	if (!__smem)
		return -EPROBE_DEFER;

	if (host < SMEM_HOST_COUNT && __smem->ptable_entries[host]) {
		entry = __smem->ptable_entries[host];
		phdr = ptable_entry_to_phdr(entry);

		ret = le32_to_cpu(phdr->offset_free_cached) -
		      le32_to_cpu(phdr->offset_free_uncached);

		if (ret > le32_to_cpu(entry->size))
			return -EINVAL;
	} else if (__smem->global_partition_entry) {
		entry = __smem->global_partition_entry;
		phdr = ptable_entry_to_phdr(entry);

		ret = le32_to_cpu(phdr->offset_free_cached) -
		      le32_to_cpu(phdr->offset_free_uncached);

		if (ret > le32_to_cpu(entry->size))
			return -EINVAL;
	} else {
		header = __smem->regions[0].virt_base;
		ret = le32_to_cpu(header->available);

		if (ret > __smem->regions[0].size)
			return -EINVAL;
	}

	return ret;
}
EXPORT_SYMBOL(qcom_smem_get_free_space);

/**
 * qcom_smem_virt_to_phys() - Convert SMEM address to physical address.
 * @smem_address	Address of SMEM item (returned by qcom_smem_get())

 * This function should only be used if an SMEM item needs to be handed off
 * to a DMA engine.
 */
phys_addr_t qcom_smem_virt_to_phys(void *addr)
{
	phys_addr_t phys_addr = 0;
	struct smem_region *area;
	void *end;
	int i;

	if (!__smem)
		return phys_addr;

	for (i = 0; i < __smem->num_regions; i++) {
		area = &__smem->regions[i];
		end = area->virt_base + area->size;
		if (addr >= area->virt_base && addr < end)
			phys_addr = addr - area->virt_base + area->aux_base;
	}

	return phys_addr;
}
EXPORT_SYMBOL(qcom_smem_virt_to_phys);

static int qcom_smem_get_sbl_version(struct qcom_smem *smem)
{
	struct smem_header *header;
	__le32 *versions;

	header = smem->regions[0].virt_base;
	versions = header->version;

	return le32_to_cpu(versions[SMEM_MASTER_SBL_VERSION_INDEX]);
}

static struct smem_ptable *qcom_smem_get_ptable(struct qcom_smem *smem)
{
	struct smem_ptable *ptable;
	u32 version;

	ptable = smem->regions[0].virt_base + smem->regions[0].size - SZ_4K;
	if (memcmp(ptable->magic, SMEM_PTABLE_MAGIC, sizeof(ptable->magic)))
		return NULL;

	version = le32_to_cpu(ptable->version);
	if (version != 1) {
		dev_err(smem->dev,
			"Unsupported partition header version %d\n", version);
		return ERR_PTR(-EINVAL);
	}
	return ptable;
}

static u32 qcom_smem_get_dynamic_item(struct qcom_smem *smem)
{
	struct smem_ptable *ptable;
	struct smem_info *info;

	ptable = qcom_smem_get_ptable(smem);
	if (IS_ERR_OR_NULL(ptable))
		return SMEM_ITEM_COUNT;

	info = (struct smem_info *)&ptable->entry[ptable->num_entries];
	if (memcmp(info->magic, SMEM_INFO_MAGIC, sizeof(info->magic)))
		return SMEM_ITEM_COUNT;

	return le16_to_cpu(info->num_items);
}

static int qcom_smem_set_global_partition(struct qcom_smem *smem)
{
	struct smem_partition_header *header;
	struct smem_ptable_entry *entry = NULL;
	struct smem_ptable *ptable;
	u32 host0, host1, size;
	int i;

	ptable = qcom_smem_get_ptable(smem);
	if (IS_ERR_OR_NULL(ptable))
		return -EINVAL;

	for (i = 0; i < le32_to_cpu(ptable->num_entries); i++) {
		entry = &ptable->entry[i];
		host0 = le16_to_cpu(entry->host0);
		host1 = le16_to_cpu(entry->host1);

		if (host0 == SMEM_GLOBAL_HOST && host0 == host1)
			break;
	}

	if (!entry) {
		dev_err(smem->dev, "Missing entry for global partition\n");
		return -EINVAL;
	}

	if (!le32_to_cpu(entry->offset) || !le32_to_cpu(entry->size)) {
		dev_err(smem->dev, "Invalid entry for global partition\n");
		return -EINVAL;
	}

	if (smem->global_partition_entry) {
		dev_err(smem->dev, "Already found the global partition\n");
		return -EINVAL;
	}

	header = smem->regions[0].virt_base + le32_to_cpu(entry->offset);
	host0 = le16_to_cpu(header->host0);
	host1 = le16_to_cpu(header->host1);

	if (memcmp(header->magic, SMEM_PART_MAGIC, sizeof(header->magic))) {
		dev_err(smem->dev, "Global partition has invalid magic\n");
		return -EINVAL;
	}

	if (host0 != SMEM_GLOBAL_HOST && host1 != SMEM_GLOBAL_HOST) {
		dev_err(smem->dev, "Global partition hosts are invalid\n");
		return -EINVAL;
	}

	if (le32_to_cpu(header->size) != le32_to_cpu(entry->size)) {
		dev_err(smem->dev, "Global partition has invalid size\n");
		return -EINVAL;
	}

	size = le32_to_cpu(header->offset_free_uncached);
	if (size > le32_to_cpu(header->size)) {
		dev_err(smem->dev,
			"Global partition has invalid free pointer\n");
		return -EINVAL;
	}

	smem->global_partition_entry = entry;

	return 0;
}

static int qcom_smem_enumerate_partitions(struct qcom_smem *smem,
					  unsigned int local_host)
{
	struct smem_partition_header *header;
	struct smem_ptable_entry *entry;
	struct smem_ptable *ptable;
	unsigned int remote_host;
	u32 host0, host1;
	int i;

	ptable = qcom_smem_get_ptable(smem);
	if (IS_ERR_OR_NULL(ptable))
		return PTR_ERR(ptable);

	for (i = 0; i < le32_to_cpu(ptable->num_entries); i++) {
		entry = &ptable->entry[i];
		host0 = le16_to_cpu(entry->host0);
		host1 = le16_to_cpu(entry->host1);

		if (host0 != local_host && host1 != local_host)
			continue;

		if (!le32_to_cpu(entry->offset))
			continue;

		if (!le32_to_cpu(entry->size))
			continue;

		if (host0 == local_host)
			remote_host = host1;
		else
			remote_host = host0;

		if (remote_host >= SMEM_HOST_COUNT) {
			dev_err(smem->dev,
				"Invalid remote host %d\n",
				remote_host);
			return -EINVAL;
		}

		if (smem->ptable_entries[remote_host]) {
			dev_err(smem->dev,
				"Already found a partition for host %d\n",
				remote_host);
			return -EINVAL;
		}

		header = smem->regions[0].virt_base + le32_to_cpu(entry->offset);
		host0 = le16_to_cpu(header->host0);
		host1 = le16_to_cpu(header->host1);

		if (memcmp(header->magic, SMEM_PART_MAGIC,
			    sizeof(header->magic))) {
			dev_err(smem->dev,
				"Partition %d has invalid magic\n", i);
			return -EINVAL;
		}

		if (host0 != local_host && host1 != local_host) {
			dev_err(smem->dev,
				"Partition %d hosts are invalid\n", i);
			return -EINVAL;
		}

		if (host0 != remote_host && host1 != remote_host) {
			dev_err(smem->dev,
				"Partition %d hosts are invalid\n", i);
			return -EINVAL;
		}

		if (le32_to_cpu(header->size) != le32_to_cpu(entry->size)) {
			dev_err(smem->dev,
				"Partition %d has invalid size\n", i);
			return -EINVAL;
		}

		if (le32_to_cpu(header->offset_free_uncached) > le32_to_cpu(header->size)) {
			dev_err(smem->dev,
				"Partition %d has invalid free pointer\n", i);
			return -EINVAL;
		}

		smem->ptable_entries[remote_host] = entry;
	}

	return 0;
}

static int qcom_smem_map_memory(struct qcom_smem *smem, struct device *dev,
				const char *name, int i)
{
	struct device_node *np;
	struct resource r;
	int ret;

	np = of_parse_phandle(dev->of_node, name, 0);
	if (!np) {
		dev_err(dev, "No %s specified\n", name);
		return -EINVAL;
	}

	ret = of_address_to_resource(np, 0, &r);
	of_node_put(np);
	if (ret)
		return ret;

	smem->regions[i].aux_base = (u32)r.start;
	smem->regions[i].size = resource_size(&r);
	smem->regions[i].virt_base = devm_ioremap_wc(dev, r.start, resource_size(&r));
	if (!smem->regions[i].virt_base)
		return -ENOMEM;

	return 0;
}

static int qcom_smem_probe(struct platform_device *pdev)
{
	struct smem_header *header;
	struct qcom_smem *smem;
	size_t array_size;
	int num_regions;
	int hwlock_id;
	u32 version;
	int ret;
	u32 host_id;

	num_regions = 1;
	if (of_find_property(pdev->dev.of_node, "qcom,rpm-msg-ram", NULL))
		num_regions++;

	array_size = num_regions * sizeof(struct smem_region);
	smem = devm_kzalloc(&pdev->dev, sizeof(*smem) + array_size, GFP_KERNEL);
	if (!smem)
		return -ENOMEM;

	smem->dev = &pdev->dev;
	smem->num_regions = num_regions;

	ret = qcom_smem_map_memory(smem, &pdev->dev, "memory-region", 0);
	if (ret)
		return ret;

	if (num_regions > 1 && (ret = qcom_smem_map_memory(smem, &pdev->dev,
					"qcom,rpm-msg-ram", 1)))
		return ret;

	ret = of_property_read_u32(pdev->dev.of_node, "smem-host-id", &host_id);
	if (!ret)
		smem_host_id = host_id;

	header = smem->regions[0].virt_base;
	if (le32_to_cpu(header->initialized) != 1 ||
	    le32_to_cpu(header->reserved)) {
		dev_err(&pdev->dev, "SMEM is not initialized by SBL\n");
		return -EINVAL;
	}

	version = qcom_smem_get_sbl_version(smem);
	switch (version >> 16) {
	case SMEM_GLOBAL_PART_VERSION:
		ret = qcom_smem_set_global_partition(smem);
		if (ret < 0)
			return ret;
		smem->item_count = qcom_smem_get_dynamic_item(smem);
		break;
	case SMEM_GLOBAL_HEAP_VERSION:
		smem->item_count = SMEM_ITEM_COUNT;
		break;
	default:
		dev_err(&pdev->dev, "Unsupported SMEM version 0x%x\n", version);
		return -EINVAL;
	}

	ret = qcom_smem_enumerate_partitions(smem, smem_host_id);
	if (ret < 0)
		return ret;

	hwlock_id = of_hwspin_lock_get_id(pdev->dev.of_node, 0);
	if (hwlock_id < 0) {
		if (hwlock_id != -EPROBE_DEFER)
			dev_err(&pdev->dev, "failed to retrieve hwlock\n");
		return hwlock_id;
	}

	smem->hwlock = hwspin_lock_request_specific(hwlock_id);
	if (!smem->hwlock)
		return -ENXIO;

	__smem = smem;

	return 0;
}

static int qcom_smem_remove(struct platform_device *pdev)
{
	hwspin_lock_free(__smem->hwlock);
	/* In case of Hibernation Restore __smem object is still valid
	 * and we call probe again so same object get allocated again
	 * that result into possible memory leak, hence explicitly freeing
	 * it here.
	 */
	devm_kfree(&pdev->dev, __smem);
	__smem = NULL;

	return 0;
}

static int qcom_smem_freeze(struct device *dev)
{
	struct platform_device *pdev = container_of(dev, struct
					platform_device, dev);
	dev_dbg(dev, "%s\n", __func__);

	qcom_smem_remove(pdev);

	return 0;
}

static int qcom_smem_restore(struct device *dev)
{
	int ret = 0;
	struct platform_device *pdev = container_of(dev, struct
					platform_device, dev);
	dev_dbg(dev, "%s\n", __func__);

	/*
	 * SMEM related information has to fetched again
	 * during resuming from Hibernation, Hence call probe.
	 */
	ret = qcom_smem_probe(pdev);
	if (ret)
		dev_err(dev, "Error getting SMEM information");

	return ret;
}

static const struct dev_pm_ops qcom_smem_pm_ops = {
	.freeze_late = qcom_smem_freeze,
	.restore_early = qcom_smem_restore,
	.thaw_early = qcom_smem_restore,
};

static const struct of_device_id qcom_smem_of_match[] = {
	{ .compatible = "qcom,smem" },
	{}
};
MODULE_DEVICE_TABLE(of, qcom_smem_of_match);

static struct platform_driver qcom_smem_driver = {
	.probe = qcom_smem_probe,
	.remove = qcom_smem_remove,
	.driver  = {
		.name = "qcom-smem",
		.of_match_table = qcom_smem_of_match,
		.suppress_bind_attrs = true,
		.pm = &qcom_smem_pm_ops,
	},
};

static int __init qcom_smem_init(void)
{
	return platform_driver_register(&qcom_smem_driver);
}
arch_initcall(qcom_smem_init);

static void __exit qcom_smem_exit(void)
{
	platform_driver_unregister(&qcom_smem_driver);
}
module_exit(qcom_smem_exit)

MODULE_AUTHOR("Bjorn Andersson <bjorn.andersson@sonymobile.com>");
MODULE_DESCRIPTION("Qualcomm Shared Memory Manager");
MODULE_LICENSE("GPL v2");
