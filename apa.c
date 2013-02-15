/*
 * apa.c
 * $Id: apa.c,v 1.11 2005/02/17 17:51:42 b081 Exp $
 *
 * Copyright 2004 Bobi B., w1zard0f07@yahoo.com
 *
 * This file is part of hdl_dump.
 *
 * hdl_dump is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * hdl_dump is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with hdl_dump; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <stdio.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>

#include "byteseq.h"
#include "retcodes.h"
#include "common.h"
#include "ps2_hdd.h"
#include "osal.h"
#include "apa.h"
#include "hio_probe.h"


#define AUTO_DELETE_EMPTY


#define _MB * (1024 * 1024) /* really ugly :-) */

typedef struct ps2_partition_run_type
{
  unsigned long sector;
  u_int32_t size_in_mb;
} ps2_partition_run_t;


static int apa_check (const apa_partition_table_t *table);


/**************************************************************/
u_int32_t
apa_partition_checksum (const ps2_partition_header_t *part)
{
  const u_int32_t *p = (const u_int32_t*) part;
  register u_int32_t i;
  u_int32_t sum = 0;
  for (i=1; i<256; ++i)
    sum += get_u32 (p + i);
  return (sum);
}


/**************************************************************/
int /* RET_OK, RET_NOT_APA, RET_ERR */
is_apa_partition (osal_handle_t handle)
{
  ps2_partition_header_t part;
  u_int32_t bytes;

  int result = osal_read (handle, &part, sizeof (part), &bytes);
  if (result == OSAL_OK)
    {
      if (bytes == sizeof (part) &&
	  memcmp (part.magic, PS2_PARTITION_MAGIC, 4) == 0 &&
	  get_u32 (&part.checksum) == apa_partition_checksum (&part))
	return (RET_OK); /* APA */
      else
	return (RET_NOT_APA); /* not APA */
    }
  else
    return (result); /* error */
}


/**************************************************************/
static apa_partition_table_t*
apa_ptable_alloc (void)
{
  apa_partition_table_t *table = osal_alloc (sizeof (apa_partition_table_t));
  if (table != NULL)
    memset (table, 0, sizeof (apa_partition_table_t));
  return (table);
}


/**************************************************************/
void
apa_ptable_free (apa_partition_table_t *table)
{
  if (table != NULL)
    {
      if (table->chunks_map != NULL)
	osal_free (table->chunks_map);
      if (table->parts != NULL)
	osal_free (table->parts);
      osal_free (table);
    }
}


/**************************************************************/
static int
apa_part_add (apa_partition_table_t *table,
	      const ps2_partition_header_t *part,
	      int existing,
	      int linked)
{
  if (table->part_count == table->part_alloc_)
    { /* grow buffer */
      u_int32_t bytes = (table->part_alloc_ + 16) * sizeof (apa_partition_t);
      apa_partition_t *tmp = osal_alloc (bytes);
      if (tmp != NULL)
	{
	  memset (tmp, 0, bytes);
	  if (table->parts != NULL) /* copy existing */
	    memcpy (tmp, table->parts, table->part_count * sizeof (apa_partition_t));
	  osal_free (table->parts);
	  table->parts = tmp;
	  table->part_alloc_ += 16;
	}
      else
	return (RET_NO_MEM);
    }

  memcpy (&table->parts [table->part_count].header, part, sizeof (ps2_partition_header_t));
  table->parts [table->part_count].existing = existing;
  table->parts [table->part_count].modified = !existing;
  table->parts [table->part_count].linked = linked;
  ++table->part_count;
  return (RET_OK);
}


/**************************************************************/
static int
apa_setup_statistics (apa_partition_table_t *table)
{
  u_int32_t i;
  char *map;

  table->total_chunks = table->device_size_in_mb / 128;
  map = osal_alloc (table->total_chunks * sizeof (char));
  if (map != NULL)
    {
      for (i=0; i<table->total_chunks; ++i)
	map [i] = MAP_AVAIL;

      /* build occupided/available space map */
      table->allocated_chunks = 0;
      table->free_chunks = table->total_chunks;
      for (i=0; i<table->part_count; ++i)
	{
	  const ps2_partition_header_t *part = &table->parts [i].header;
	  u_int32_t part_no = get_u32 (&part->start) / ((128 _MB) / 512);
	  u_int32_t num_parts = get_u32 (&part->length) / ((128 _MB) / 512);

	  /* "alloc" num_parts starting at part_no */
	  while (num_parts)
	    {
	      if (map [part_no] == MAP_AVAIL)
		map [part_no] = get_u32 (&part->main) == 0 ? MAP_MAIN : MAP_SUB;
	      else
		map [part_no] = MAP_COLL; /* collision */
	      ++part_no;
	      --num_parts;
	      ++table->allocated_chunks;
	      --table->free_chunks;
	    }
	}

      if (table->chunks_map != NULL)
	osal_free (table->chunks_map);
      table->chunks_map = map;

      return (RET_OK);
    }
  else
    return (RET_NO_MEM);
}


/**************************************************************/
int
apa_ptable_read (const char *path,
		 apa_partition_table_t **table)
{
  hio_t *hio;
  int result = hio_probe (path, &hio); /* do not disable caching */
  if (result == OSAL_OK)
    {
      result = apa_ptable_read_ex (hio, table);
      hio->close (hio);
    }
  return (result);
}


/**************************************************************/
int
apa_ptable_read_ex (hio_t *hio,
		    apa_partition_table_t **table)
{
  u_int32_t size_in_kb;
  int result = hio->stat (hio, &size_in_kb);
  if (result == OSAL_OK)
    {
      u_int32_t total_sectors;
#if defined (LIMIT_HDD_TO_128GB)
      /* limit HDD size to 128GB - 1KB; that is: exclude the last 128MB chunk */
      if (size_in_kb > 128 * 1024 * 1024 - 1)
	size_in_kb = 128 * 1024 * 1024 - 1;
#endif
      total_sectors = size_in_kb * 2; /* 1KB = 2 sectors of 512 bytes, each */

      *table = apa_ptable_alloc ();
      if (table != NULL)
	{
	  u_int32_t sector = 0;
	  do
	    {
	      u_int32_t bytes;
	      ps2_partition_header_t part;
	      result = hio->read (hio, sector, sizeof (part) / 512, &part, &bytes);
	      if (result == OSAL_OK)
		{
		  if (bytes == sizeof (part) &&
		      get_u32 (&part.checksum) == apa_partition_checksum (&part) &&
		      memcmp (part.magic, PS2_PARTITION_MAGIC, 4) == 0)
		    {
		      if (get_u32 (&part.start) < total_sectors &&
			  get_u32 (&part.start) + get_u32 (&part.length) < total_sectors)
			{
			  result = apa_part_add (*table, &part, 1, 1);
			  if (result == RET_OK)
			    sector = get_u32 (&part.next);
			}
		      else
			{ /* partition behind end-of-HDD */
#if defined (LIMIT_HDD_TO_128GB)
			  result = RET_CROSS_128GB; /* data behind 128GB mark */
#else
			  result = RET_BAD_APA; /* data behind end-of-HDD */
#endif
			  break;
			}
		    }
		  else
		    result = RET_NOT_APA;
		}
	    }
	  while (result == RET_OK && sector != 0);

	  if (result == RET_OK)
	    {
	      (*table)->device_size_in_mb = size_in_kb / 1024;
	      result = apa_setup_statistics (*table);
	      if (result == RET_OK)
		result = apa_check (*table);
	    }

#if defined (AUTO_DELETE_EMPTY)
	  if (result == RET_OK)
	    { /* automatically delete "__empty" partitions */
	      do
		{
		  result = apa_delete_partition (*table, "__empty");
		}
	      while (result == RET_OK);
	      if (result == RET_NOT_FOUND)
		result = apa_check (*table);
	    }
#endif

	  if (result != RET_OK)
	    apa_ptable_free (*table);
	}
      else
	result = RET_NO_MEM;
    }
  return (result);
}


/**************************************************************/
int
apa_find_partition (const apa_partition_table_t *table,
		    const char *partition_name,
		    u_int32_t *partition_index)
{
  u_int32_t i;
  int result = RET_NOT_FOUND;
  for (i=0; i<table->part_count; ++i)
    {
      const ps2_partition_header_t *part = &table->parts [i].header;
      if (get_u32 (&part->main) == 0)
	{ /* trim partition name */
	  char id_copy [PS2_PART_IDMAX + 1];
	  char *part_id_end = id_copy + PS2_PART_IDMAX - 1;
	  memcpy (id_copy, part->id, PS2_PART_IDMAX);
	  id_copy [PS2_PART_IDMAX] = '\0';
	  while (part_id_end > id_copy &&
		 *part_id_end == ' ')
	    *part_id_end-- = '\0';
	  if (caseless_compare (id_copy, partition_name))
	    { /* found */
	      *partition_index = i;
	      result = RET_OK;
	      break;
	    }
	}
    }
  return (result);
}


/**************************************************************/
static int
compare_partitions (const void *e1,
		    const void *e2)
{
  const ps2_partition_run_t *p1 = e1;
  const ps2_partition_run_t *p2 = e2;
  int diff = (int) p2->size_in_mb - (int) p1->size_in_mb;
  if (diff != 0)
    return (diff);
  else
    return ((int) p1->sector - (int) p2->sector);
}


/* descending by size, ascending by sector */
static void
sort_partitions (ps2_partition_run_t *partitions,
		 u_int32_t partitions_used)
{
  /* TODO: consider better sorting to take care about partitioning on as few runs as possible */
  qsort (partitions, partitions_used, sizeof (ps2_partition_run_t), &compare_partitions);
}


/* join neighbour partitions of the same size, but sum up to max_part_size_in_mb */
static void
optimize_partitions (ps2_partition_run_t *partitions,
		     u_int32_t *partitions_used,
		     u_int32_t max_part_size_in_mb)
{
  int have_join;
  do
    {
      u_int32_t i;
      have_join = 0;
      for (i=0; i<*partitions_used - 1; ++i)
	{
	  u_int32_t curr_part_end_sector = 
	    partitions [i].sector +
	    partitions [i].size_in_mb * ((1 _MB) / 512);
	  u_int32_t u_int32_to_be = partitions [i].size_in_mb * 2;
	  int next_is_same_size =
	    partitions [i].size_in_mb == partitions [i + 1].size_in_mb;
	  int would_be_aligned =
	    partitions [i].sector % (u_int32_to_be * ((1 _MB) / 512)) == 0;

	  if (u_int32_to_be <= max_part_size_in_mb &&
	      curr_part_end_sector == partitions [i + 1].sector &&
	      next_is_same_size &&
	      would_be_aligned)
	    {
	      partitions [i].size_in_mb *= 2;
	      memmove (partitions + i + 1, partitions + i + 2,
		       (*partitions_used - i - 2) * sizeof (ps2_partition_run_t));
	      --(*partitions_used);
	      have_join = 1;
	    }
	}
    }
  while (have_join);
}


/**************************************************************/
static void
set_ps2fs_datetime (ps2fs_datetime_t *dt,
		    time_t to)
{
  struct tm tm;
  memcpy (&tm, localtime (&to), sizeof (struct tm));
  dt->unused = 0;
  dt->sec = (u_int8_t) tm.tm_sec;
  dt->min = (u_int8_t) tm.tm_min;
  dt->hour = (u_int8_t) tm.tm_hour;
  dt->day = (u_int8_t) tm.tm_mday;
  dt->month = (u_int8_t) (tm.tm_mon + 1);
  set_u16 (&dt->year, (u_int16_t) (tm.tm_year + 1900));
}


static void
setup_main_part (ps2_partition_header_t *part,
		 const char *name,
		 const ps2_partition_run_t *partitions,
		 u_int32_t partitions_used,
		 u_int32_t last_partition_sector)
{
  u_int32_t i;
  memset (part, 0, sizeof (ps2_partition_header_t));
  memcpy (part->magic, PS2_PARTITION_MAGIC, 4);
  set_u32 (&part->next, partitions_used > 0 ? partitions [1].sector : 0);
  set_u32 (&part->prev, last_partition_sector);
  memcpy (part->id, name,
	  strlen (name) > PS2_PART_IDMAX ? PS2_PART_IDMAX : strlen (name));
  set_u32 (&part->start, partitions [0].sector);
  set_u32 (&part->length, partitions [0].size_in_mb * ((1 _MB) / 512));
  set_u16 (&part->type, 0x1337);
  set_u16 (&part->flags, 0);
  set_u32 (&part->nsub, partitions_used - 1);
  set_ps2fs_datetime (&part->created, time (NULL));
  set_u32 (&part->main, 0);
  set_u32 (&part->number, 0);
  set_u16 (&part->unknown2, 513);
  for (i=1; i<partitions_used; ++i)
    {
      set_u32 (&part->subs [i - 1].start, partitions [i].sector);
      set_u32 (&part->subs [i - 1].length, partitions [i].size_in_mb * ((1 _MB) / 512));
    }
  set_u32 (&part->checksum, apa_partition_checksum (part));
}


static void
setup_sub_part (ps2_partition_header_t *part,
		u_int32_t index,
		const ps2_partition_run_t *partitions,
		u_int32_t partitions_used)
{
  memset (part, 0, sizeof (ps2_partition_header_t));
  memcpy (part->magic, PS2_PARTITION_MAGIC, 4);
  set_u32 (&part->next, index + 1 < partitions_used ? partitions [index + 1].sector : 0);
  set_u32 (&part->prev, partitions [index - 1].sector);
  set_u32 (&part->start, partitions [index].sector);
  set_u32 (&part->length, partitions [index].size_in_mb * ((1 _MB) / 512));
  set_u16 (&part->type, 0x1337);
  set_u16 (&part->flags, PS2_PART_FLAG_SUB);
  set_u32 (&part->nsub, 0);
  set_ps2fs_datetime (&part->created, time (NULL));
  set_u32 (&part->main, partitions [0].sector);
  set_u32 (&part->number, index);
  set_u16 (&part->unknown2, 513);
  set_u32 (&part->checksum, apa_partition_checksum (part));
}


/**************************************************************/
static int
sort_by_starting_sector (const void *e1,
			 const void *e2)
{
  const apa_partition_t *p1 = (const apa_partition_t*) e1;
  const apa_partition_t *p2 = (const apa_partition_t*) e2;
  return (get_u32 (&p1->header.start) > get_u32 (&p2->header.start) ? 1 : -1);
}

static void
normalize_linked_list (apa_partition_table_t *table)
{
  qsort (table->parts, table->part_count, sizeof (apa_partition_t), sort_by_starting_sector);
  if (table->part_count >= 1)
    {
      u_int32_t i;
      for (i=0; i<table->part_count; ++i)
	{
	  apa_partition_t *prev = table->parts + (i > 0 ? i - 1 : table->part_count - 1);
	  apa_partition_t *curr = table->parts + i;
	  apa_partition_t *next = table->parts + (i + 1 < table->part_count ? i + 1 : 0);
	  /* no need to be endian-aware, since both have same endianess */
	  if (curr->header.prev != prev->header.start)
	    {
	      curr->modified = 1;
	      curr->header.prev = prev->header.start;
	    }
	  if (curr->header.next != next->header.start)
	    {
	      curr->modified = 1;
	      curr->header.next = next->header.start;
	    }
	  if (curr->modified)
	    set_u32 (&curr->header.checksum, apa_partition_checksum (&curr->header));
	}
    }
}


/**************************************************************/
int
apa_allocate_space (apa_partition_table_t *table,
		    const char *partition_name,
		    u_int32_t size_in_mb,
		    u_int32_t *new_partition_start,
		    int decreasing_size)
{
  int result = RET_OK;
  char *map = table->chunks_map;
  u_int32_t i;
  int found;

  /* check whether that partition name is not already used */
  found = 0;
  for (i=0; i<table->part_count; ++i)
    if (get_u16 (&table->parts [i].header.flags) == 0 &&
	get_u32 (&table->parts [i].header.main) == 0)
      if (caseless_compare (partition_name, table->parts [i].header.id))
	{
	  found = 1;
	  break;
	}
  if (found)
    return (RET_PART_EXISTS);

  if (table->free_chunks * 128 >= size_in_mb)
    {
      u_int32_t max_part_size_in_entries = table->total_chunks < 32 ? 1 : table->total_chunks / 32;
      u_int32_t max_part_size_in_mb = max_part_size_in_entries * 128;
      u_int32_t estimated_entries = (size_in_mb + 127) / 128 + 1;
      u_int32_t partitions_used = 0;
      ps2_partition_run_t *partitions =
	osal_alloc (estimated_entries * sizeof (ps2_partition_run_t));
      if (partitions != NULL)
	{
	  /* use the most straight forward approach possible:
	     fill from the first gap onwards */
	  u_int32_t mb_remaining = size_in_mb;
	  u_int32_t allocated_mb, overhead_mb;
	  for (i=0; i<estimated_entries; ++i)
	    { /* initialize */
	      partitions [i].sector = 0;
	      partitions [i].size_in_mb = 0;
	    }
	  for (i=0; mb_remaining>0 && i<table->total_chunks; ++i)
	    if (map [i] == MAP_AVAIL)
	      {
		partitions [partitions_used].sector = i * ((128 _MB) / 512);
		partitions [partitions_used].size_in_mb = 128;
		map [i] = MAP_ALLOC; /* "allocate" chunk */
		++(partitions_used);
		mb_remaining = (mb_remaining > 128 ? mb_remaining - 128 : 0);
	      }

	  optimize_partitions (partitions, &partitions_used, max_part_size_in_mb);

	  /* calculate overhead (4M for main + 1M for each sub)
	     and allocate additional 128 M partition if necessary */
	  allocated_mb = 0; overhead_mb = 3;
	  for (i=0; i<partitions_used; ++i)
	    {
	      allocated_mb += partitions [i].size_in_mb;
	      ++overhead_mb;
	    }
	  if (allocated_mb < size_in_mb + overhead_mb)
	    { /* add one more partition or return RET_NO_SPACE */
	      int free_entry_found = 0; /* if free entry not found return RET_NO_SPACE */
	      for (i=0; i<table->total_chunks; ++i)
		if (map [i] == MAP_AVAIL)
		  {
		    partitions [partitions_used].sector = i * ((128 _MB) / 512);
		    partitions [partitions_used].size_in_mb = 128;
		    ++partitions_used;
		    optimize_partitions (partitions, &partitions_used, max_part_size_in_mb);

		    free_entry_found = 1;
		    break;
		  }
	      result = free_entry_found ? RET_OK : RET_NO_SPACE;
	    }
	  else
	    result = RET_OK;

	  if (result == RET_OK)
	    { /* create new partitions in the partition table */
	      ps2_partition_header_t part;
	      u_int32_t last_partition_start =
		table->part_count > 0 ?
		get_u32 (&table->parts [table->part_count - 1].header.start) : 0;

	      if (decreasing_size)
		sort_partitions (partitions, partitions_used);

	      /* current last partition would be remapped by normalize */
	      setup_main_part (&part, partition_name, partitions, partitions_used,
			       last_partition_start);
	      result = apa_part_add (table, &part, 0, 1);
	      for (i=1; result == RET_OK && i<partitions_used; ++i)
		{
		  setup_sub_part (&part, i, partitions, partitions_used);
		  result = apa_part_add (table, &part, 0, 1);
		}
	      if (result == RET_OK)
		{
		  normalize_linked_list (table);
		  *new_partition_start = partitions [0].sector;
		}
	    }
	  osal_free (partitions);
	}
      else
	result = RET_NO_MEM; /* out-of-memory */
    }
  else
    result = RET_NO_SPACE; /* not enough free space */

  return (result);
}


/**************************************************************/
int
apa_delete_partition (apa_partition_table_t *table,
		      const char *partition_name)
{
  u_int32_t partition_index;
  int result = apa_find_partition (table, partition_name, &partition_index);
  if (result == RET_OK)
    {
      u_int32_t i, count = 1;
      u_int32_t pending_deletes [PS2_PART_MAXSUB]; /* starting sectors of parts pending delete */
      const ps2_partition_header_t *part = &table->parts [partition_index].header;

      if (get_u16 (&part->type) == 1)
	return (RET_NOT_ALLOWED); /* deletion of system partitions is not allowed */

      /* preserve a list of partitions to be deleted */
      pending_deletes [0] = get_u32 (&part->start);
      for (i=0; i<get_u32 (&part->nsub); ++i)
	pending_deletes [count++] = get_u32 (&part->subs [i].start);

      /* remove partitions from the double-linked list */
      i = 0;
      while (i < table->part_count)
	{
	  u_int32_t j;
	  int found = 0;
	  for (j=0; j<count; ++j)
	    if (get_u32 (&table->parts [i].header.start) == pending_deletes [j])
	      {
		found = 1;
		break;
	      }
	  if (found)
	    { /* remove this partition */
	      u_int32_t part_no = get_u32 (&table->parts [i].header.start) / 262144; /* 262144 sectors == 128M */
	      u_int32_t num_parts = get_u32 (&table->parts [i].header.length) / 262144;

	      memmove (table->parts + i, table->parts + i + 1,
		       sizeof (apa_partition_t) * (table->part_count - i - 1));
	      --table->part_count;

	      /* "free" num_parts starting at part_no */
	      while (num_parts)
		{
		  table->chunks_map [part_no] = MAP_AVAIL;
		  ++part_no;
		  --num_parts;
		  --table->allocated_chunks;
		  ++table->free_chunks;
		}
	    }
	  else
	    ++i;
	}

      normalize_linked_list (table);
    }
  return (result);
}


/**************************************************************/
int
apa_commit (const char *path,
	    const apa_partition_table_t *table)
{
  int result = apa_check (table);
  if (result == RET_OK)
    {
      hio_t *hio;
      result = hio_probe (path, &hio);
      if (result == OSAL_OK)
	{
	  result = apa_commit_ex (hio, table);
	  result = hio->close (hio) == OSAL_OK ? result : OSAL_ERR;
	}
    }
  return (result);
}


/**************************************************************/
int
apa_commit_ex (hio_t *hio,
	       const apa_partition_table_t *table)
{
  int result = apa_check (table);
  if (result == RET_OK)
    {
      u_int32_t i;
      for (i=0; result == RET_OK && i<table->part_count; ++i)
	{
	  if (table->parts [i].modified)
	    {
	      u_int32_t bytes;
	      const ps2_partition_header_t *part = &table->parts [i].header;
	      result = hio->write (hio, get_u32 (&part->start),
				   sizeof (*part) / 512, part, &bytes);
	      if (result == OSAL_OK)
		result = bytes == sizeof (ps2_partition_header_t) ? OSAL_OK : RET_ERR;
	    }
	}
    }
  return (result);
}


/**************************************************************/
#define ADD_PROBLEM(buff,size,problem,len) \
  if ((len) < (size))			   \
    {					   \
      memcpy (buff, problem, len);	   \
      (size) -= (len);			   \
      (buff) += (len);			   \
       *(buff) = '\0';			   \
    }

static int
apa_list_problems (const apa_partition_table_t *table,
		   char *buffer,
		   size_t buffer_size)
{ /* NOTE: keep in sync with apa_check */
  u_int32_t i, j, k;
  char tmp [1024];
  size_t len;

  const u_int32_t total_sectors = table->device_size_in_mb * 1024 * 2;

  *buffer = '\0';
  for (i=0; i<table->part_count; ++i)
    {
      const ps2_partition_header_t *part = &table->parts [i].header;
      u_int32_t checksum = apa_partition_checksum (part);
      if (get_u32 (&part->checksum) != checksum)
	{
	  len = sprintf (tmp, "%06lx00: bad checksum: 0x%08lx instead of 0x%08lx;\n",
			 (unsigned long) (get_u32 (&part->start) >> 8),
			 (unsigned long) get_u32 (&part->checksum),
			 (unsigned long) checksum);
	  ADD_PROBLEM (buffer, buffer_size, tmp, len);
	}

      if (get_u32 (&part->start) < total_sectors &&
	  get_u32 (&part->start) + get_u32 (&part->length) <= total_sectors)
	;
      else
	{
#if defined (LIMIT_HDD_TO_128GB)
	  len = sprintf (tmp, "%06lx00 +%06lx00: across 128GB mark;\n",
			 (unsigned long) (get_u32 (&part->start) >> 8),
			 (unsigned long) (get_u32 (&part->length) >> 8));
	  ADD_PROBLEM (buffer, buffer_size, tmp, len);
#else
	  len = sprintf (tmp, "%06lx00 +%06lx00: outside HDD data area;\n",
			 (unsigned long) (get_u32 (&part->start) >> 8),
			 (unsigned long) (get_u32 (&part->length) >> 8));
	  ADD_PROBLEM (buffer, buffer_size, tmp, len);
#endif
	}

      if ((get_u32 (&part->length) % ((128 _MB) / 512)) != 0)
	{
	  len = sprintf (tmp, "%06lx00: size %06lx00 not multiple to 128MB;\n",
			 (unsigned long) (get_u32 (&part->start) >> 8),
			 (unsigned long) (get_u32 (&part->length) >> 8));
	  ADD_PROBLEM (buffer, buffer_size, tmp, len);
	}

      if ((get_u32 (&part->start) % get_u32 (&part->length)) != 0)
	{
	  len = sprintf (tmp, "%06lx00: start not multiple to size %06lx00;\n",
			 (unsigned long) (get_u32 (&part->start) >> 8),
			 (unsigned long) (get_u32 (&part->length) >> 8));
	  ADD_PROBLEM (buffer, buffer_size, tmp, len);
	}

      if (get_u32 (&part->main) == 0 &&
	  get_u16 (&part->flags) == 0 &&
	  get_u32 (&part->start) != 0)
	{ /* check sub-partitions */
	  u_int32_t count = 0;
	  for (j=0; j<table->part_count; ++j)
	    {
	      const ps2_partition_header_t *part2 = &table->parts [j].header;
	      if (get_u32 (&part2->main) == get_u32 (&part->start))
		{ /* sub-partition of current main partition */
		  int found;
		  if (get_u16 (&part2->flags) != PS2_PART_FLAG_SUB)
		    {
		      len = sprintf (tmp, "%06lx00: mismatching sub-partition flag;\n",
				     (unsigned long) (get_u32 (&part2->start) >> 8));
		      ADD_PROBLEM (buffer, buffer_size, tmp, len);
		    }

		  found = 0;
		  for (k=0; k<get_u32 (&part->nsub); ++k)
		    if (get_u32 (&part->subs [k].start) == get_u32 (&part2->start))
		      { /* in list */
			if (get_u32 (&part->subs [k].length) != get_u32 (&part2->length))
			  {
			    len = sprintf (tmp, "%06lx00: mismatching sub-partition size: %06lx00 != %06lx00;\n",
					   (unsigned long) (get_u32 (&part2->start) >> 8),
					   (unsigned long) (get_u32 (&part2->length) >> 8),
					   (unsigned long) (get_u32 (&part->subs [k].length) >> 8));
			    ADD_PROBLEM (buffer, buffer_size, tmp, len);
			  }
			found = 1;
			break;
		      }
		  if (!found)
		    {
		      len = sprintf (tmp, "%06lx00: not a sub-partition of %06lx00;\n",
				     (unsigned long) (get_u32 (&part2->start) >> 8),
				     (unsigned long) (get_u32 (&part->start) >> 8));
		      ADD_PROBLEM (buffer, buffer_size, tmp, len);
		    }

		  ++count;
		}
	    }
	  if (count != get_u32 (&part->nsub))
	    {
	      len = sprintf (tmp, "%06lx00: only %u sub-partitions found of %u;\n",
			     (unsigned long) (get_u32 (&part->start) >> 8),
			     (unsigned int) count, (unsigned int) get_u32 (&part->nsub));
	      ADD_PROBLEM (buffer, buffer_size, tmp, len);
	    }
	}
    }

  /* verify double-linked list */
  for (i=0; i<table->part_count; ++i)
    {
      apa_partition_t *prev = table->parts + (i > 0 ? i - 1 : table->part_count - 1);
      apa_partition_t *curr = table->parts + i;
      apa_partition_t *next = table->parts + (i + 1 < table->part_count ? i + 1 : 0);
      if (get_u32 (&curr->header.prev) != get_u32 (&prev->header.start))
	{
	  len = sprintf (tmp, "%06lx00: %06lx00 is previous, not %06lx00;\n",
			 (unsigned long) (get_u32 (&curr->header.start) >> 8),
			 (unsigned long) (get_u32 (&prev->header.start) >> 8),
			 (unsigned long) (get_u32 (&curr->header.prev) >> 8));
	  ADD_PROBLEM (buffer, buffer_size, tmp, len);
	}
      if (get_u32 (&curr->header.next) != get_u32 (&next->header.start))
	{
	  len = sprintf (tmp, "%06lx00: %06lx00 is next, not %06lx00;\n",
			 (unsigned long) (get_u32 (&curr->header.start) >> 8),
			 (unsigned long) (get_u32 (&next->header.start) >> 8),
			 (unsigned long) (get_u32 (&curr->header.next) >> 8));
	  ADD_PROBLEM (buffer, buffer_size, tmp, len);
	}
	return (RET_BAD_APA); /* bad links */
    }

  return (RET_OK);
}


/**************************************************************/
static int
apa_check (const apa_partition_table_t *table)
{ /* NOTE: keep in sync with apa_list_problems */
  u_int32_t i, j, k;

  const u_int32_t total_sectors = table->device_size_in_mb * 1024 * 2;

  for (i=0; i<table->part_count; ++i)
    {
      const ps2_partition_header_t *part = &table->parts [i].header;
      if (get_u32 (&part->checksum) != apa_partition_checksum (part))
	return (RET_BAD_APA); /* bad checksum */

      if (get_u32 (&part->start) < total_sectors &&
	  get_u32 (&part->start) + get_u32 (&part->length) <= total_sectors)
	;
      else
	{
#if defined (LIMIT_HDD_TO_128GB)
	  return (RET_CROSS_128GB); /* data behind 128GB mark */
#else
	  return (RET_BAD_APA); /* data behind end-of-HDD */
#endif
	}

      if ((get_u32 (&part->length) % ((128 _MB) / 512)) != 0)
	return (RET_BAD_APA); /* partition size not multiple to 128MB */

      if ((get_u32 (&part->start) % get_u32 (&part->length)) != 0)
	return (RET_BAD_APA); /* partition start not multiple on partition size */

      if (get_u32 (&part->main) == 0 &&
	  get_u16 (&part->flags) == 0 &&
	  get_u32 (&part->start) != 0)
	{ /* check sub-partitions */
	  u_int32_t count = 0;
	  for (j=0; j<table->part_count; ++j)
	    {
	      const ps2_partition_header_t *part2 = &table->parts [j].header;
	      if (get_u32 (&part2->main) == get_u32 (&part->start))
		{ /* sub-partition of current main partition */
		  int found;
		  if (get_u16 (&part2->flags) != PS2_PART_FLAG_SUB)
		    return (RET_BAD_APA);

		  found = 0;
		  for (k=0; k<get_u32 (&part->nsub); ++k)
		    if (get_u32 (&part->subs [k].start) == get_u32 (&part2->start))
		      { /* in list */
			if (get_u32 (&part->subs [k].length) != get_u32 (&part2->length))
			  return (RET_BAD_APA);
			found = 1;
			break;
		      }
		  if (!found)
		    return (RET_BAD_APA); /* not found in the list */

		  ++count;
		}
	    }
	  if (count != get_u32 (&part->nsub))
	    return (RET_BAD_APA); /* wrong number of sub-partitions */
	}
    }

  /* verify double-linked list */
  for (i=0; i<table->part_count; ++i)
    {
      apa_partition_t *prev = table->parts + (i > 0 ? i - 1 : table->part_count - 1);
      apa_partition_t *curr = table->parts + i;
      apa_partition_t *next = table->parts + (i + 1 < table->part_count ? i + 1 : 0);
      if (get_u32 (&curr->header.prev) != get_u32 (&prev->header.start) ||
	  get_u32 (&curr->header.next) != get_u32 (&next->header.start))
	return (RET_BAD_APA); /* bad links */
    }

  return (RET_OK);
}