/****************************************************************************
 * sched/irq/irq_procfs.c
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/stat.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <assert.h>
#include <debug.h>
#include <errno.h>

#include <nuttx/kmalloc.h>
#include <nuttx/fs/fs.h>
#include <nuttx/fs/procfs.h>

#include "irq/irq.h"

#if !defined(CONFIG_DISABLE_MOUNTPOINT) && defined(CONFIG_FS_PROCFS)
#ifdef CONFIG_SCHED_IRQMONITOR

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Output format:
 *
 *            1111111111222222222233333333334444444444
 *   1234567890123456789012345678901234567890123456789
 *
 *   IRQ HANDLER  ARGUMENT    COUNT    RATE    TIME
 *   DDD XXXXXXXX XXXXXXXX DDDDDDDDDD DDDD.DDD DDDD
 *
 * NOTE:  This assumes that an address can be represented in 32-bits.  In
 * the typical configuration where CONFIG_HAVE_LONG_LONG=y, the COUNT field
 * may not be wide enough.
 */

#define HDR_FMT "IRQ HANDLER  ARGUMENT    COUNT    RATE    TIME\n"
#define IRQ_FMT "%3u %08lx %08lx %10lu %4lu.%03lu %4lu\n"

/* Determines the size of an intermediate buffer that must be large enough
 * to handle the longest line generated by this logic (plus a couple of
 * bytes).
 */

#define IRQ_LINELEN 50

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* This structure describes one open "file" */

struct irq_file_s
{
  struct procfs_file_s base;  /* Base open file structure */
  FAR char *buffer;           /* User provided buffer */
  size_t remaining;           /* Number of available characters in buffer */
  size_t ncopied;             /* Number of characters in buffer */
  off_t offset;               /* Current file offset */
  char line[IRQ_LINELEN];     /* Pre-allocated buffer for formatted lines */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

/* irq_foreach() callback function */

static int     irq_callback(int irq, FAR struct irq_info_s *info,
                 FAR void *arg);

/* File system methods */

static int     irq_open(FAR struct file *filep, FAR const char *relpath,
                 int oflags, mode_t mode);
static int     irq_close(FAR struct file *filep);
static ssize_t irq_read(FAR struct file *filep, FAR char *buffer,
                 size_t buflen);
static int     irq_dup(FAR const struct file *oldp,
                 FAR struct file *newp);
static int     irq_stat(FAR const char *relpath, FAR struct stat *buf);

/****************************************************************************
 * Public Data
 ****************************************************************************/

/* See fs_mount.c -- this structure is explicitly extern'ed there.
 * We use the old-fashioned kind of initializers so that this will compile
 * with any compiler.
 */

const struct procfs_operations g_irq_operations =
{
  irq_open,       /* open */
  irq_close,      /* close */
  irq_read,       /* read */
  NULL,           /* write */

  irq_dup,        /* dup */

  NULL,           /* opendir */
  NULL,           /* closedir */
  NULL,           /* readdir */
  NULL,           /* rewinddir */

  irq_stat        /* stat */
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: irq_callback
 ****************************************************************************/

static int irq_callback(int irq, FAR struct irq_info_s *info,
                        FAR void *arg)
{
  FAR struct irq_file_s *irqfile = (FAR struct irq_file_s *)arg;
  struct irq_info_s copy;
  struct timespec delta;
  irqstate_t flags;
  clock_t elapsed;
  clock_t now;
  size_t linesize;
  size_t copysize;
  unsigned long intpart;
  unsigned long fracpart;
  unsigned long count;

  DEBUGASSERT(irqfile != NULL);

  /* Take a snapshot and reset the counts */

  flags = enter_critical_section();
  memcpy(&copy, info, sizeof(struct irq_info_s));
  now           = clock_systime_ticks();
  info->start   = now;
#ifdef CONFIG_HAVE_LONG_LONG
  info->count   = 0;
#else
  info->mscount = 0;
  info->lscount = 0;
#endif
  info->time    = 0;
  leave_critical_section(flags);

  /* Don't bother if count == 0.
   *
   * REVISIT:  There is a logic problem with skipping if the count is zero.
   * Normally this is a good thing because it makes the output concise.
   * However, it can be a problem under certain conditions:
   *
   * It may take multiple passes through the IRQ table to enumerate the
   * interrupts if the number of interrupts reported is large or if the size
   * of the user buffer is small.  If a count is zero it will be skipped on
   * the first time through but if it becomes non-zero on the second time
   * through, the output will be corrupted.  Similarly if the count is non-
   * zero the first time through and zero the second.
   *
   * A proper fix would require keep better track of where we left off
   * between passes.  Current that position is remembered only by the
   * byte offset into the pseudo-file, f_pos.
   */

  if (copy.count == 0)
    {
      return 0;
    }

  /* Calculate the interrupt rate from the interrupt count and the elapsed
   * time.
   *
   * REVISIT: If these counts have not been samples and reset in a long time
   * then the following will likely overflow.
   */

  elapsed = now - copy.start;
  up_perf_convert(copy.time, &delta);

#ifdef CONFIG_HAVE_LONG_LONG
  /* elapsed = <current-time> - <start-time>, units=clock ticks
   * rate    = <interrupt-count> * TICKS_PER_SEC / elapsed
   */

  elapsed = elapsed ? elapsed : 1;
  intpart = (unsigned int)((copy.count * TICK_PER_SEC) / elapsed);
  if (intpart >= 10000)
    {
      intpart  = 9999;
      fracpart = 999;
    }
  else
    {
      uint64_t intcount = ((uint64_t)intpart * elapsed);
      fracpart = (unsigned int)
        (((copy.count * TICK_PER_SEC - intcount) * 1000) / elapsed);
    }

  /* Make sure that the count is representable with snprintf format */

  if (copy.count > ULONG_MAX)
    {
      count = ULONG_MAX;
    }
  else
    {
      count = (unsigned long)copy.count;
    }
#else
#  error Missing logic
#endif

  /* Output information about this interrupt */

  linesize = snprintf(irqfile->line, IRQ_LINELEN, IRQ_FMT,
                      (unsigned int)irq,
                      (unsigned long)((uintptr_t)copy.handler),
                      (unsigned long)((uintptr_t)copy.arg),
                      count, intpart, fracpart,
                      (unsigned long)delta.tv_nsec / 1000);

  copysize  = procfs_memcpy(irqfile->line, linesize, irqfile->buffer,
                            irqfile->remaining, &irqfile->offset);

  irqfile->ncopied   += copysize;
  irqfile->buffer    += copysize;
  irqfile->remaining -= copysize;

  /* Return a non-zero value to stop the traversal if the user-provided
   * buffer is full.
   */

  if (irqfile->remaining > 0)
    {
      return 0;
    }
  else
    {
      return 1;
    }
}

/****************************************************************************
 * Name: irq_open
 ****************************************************************************/

static int irq_open(FAR struct file *filep, FAR const char *relpath,
                    int oflags, mode_t mode)
{
  FAR struct irq_file_s *irqfile;

  finfo("Open '%s'\n", relpath);

  /* This PROCFS file is read-only.  Any attempt to open with write access
   * is not permitted.
   */

  if ((oflags & O_WRONLY) != 0 || (oflags & O_RDONLY) == 0)
    {
      ferr("ERROR: Only O_RDONLY supported\n");
      return -EACCES;
    }

  /* Allocate a container to hold the file attributes */

  irqfile = (FAR struct irq_file_s *)kmm_zalloc(sizeof(struct irq_file_s));
  if (!irqfile)
    {
      ferr("ERROR: Failed to allocate file attributes\n");
      return -ENOMEM;
    }

  /* Save the attributes as the open-specific state in filep->f_priv */

  filep->f_priv = (FAR void *)irqfile;
  return OK;
}

/****************************************************************************
 * Name: irq_close
 ****************************************************************************/

static int irq_close(FAR struct file *filep)
{
  FAR struct irq_file_s *irqfile;

  /* Recover our private data from the struct file instance */

  irqfile = (FAR struct irq_file_s *)filep->f_priv;
  DEBUGASSERT(irqfile);

  /* Release the file attributes structure */

  kmm_free(irqfile);
  filep->f_priv = NULL;
  return OK;
}

/****************************************************************************
 * Name: irq_read
 ****************************************************************************/

static ssize_t irq_read(FAR struct file *filep, FAR char *buffer,
                        size_t buflen)
{
  FAR struct irq_file_s *irqfile;
  size_t linesize;
  size_t copysize;

  finfo("buffer=%p buflen=%d\n", buffer, (int)buflen);

  /* Recover our private data from the struct file instance */

  irqfile = (FAR struct irq_file_s *)filep->f_priv;
  DEBUGASSERT(irqfile);

  /* Save the file offset and the user buffer information */

  irqfile->offset    = filep->f_pos;
  irqfile->buffer    = buffer;
  irqfile->remaining = buflen;

  /* The first line to output is the header */

  linesize = snprintf(irqfile->line, IRQ_LINELEN, HDR_FMT);

  copysize = procfs_memcpy(irqfile->line, linesize, irqfile->buffer,
                           irqfile->remaining, &irqfile->offset);

  irqfile->ncopied    = copysize;
  irqfile->buffer    += copysize;
  irqfile->remaining -= copysize;

  /* Now traverse the list of attached interrupts, generating output for
   * each.
   */

  irq_foreach(irq_callback, (FAR void *)irqfile);

  /* Update the file position */

  filep->f_pos += irqfile->ncopied;
  return irqfile->ncopied;
}

/****************************************************************************
 * Name: irq_dup
 *
 * Description:
 *   Duplicate open file data in the new file structure.
 *
 ****************************************************************************/

static int irq_dup(FAR const struct file *oldp, FAR struct file *newp)
{
  FAR struct irq_file_s *oldattr;
  FAR struct irq_file_s *newattr;

  finfo("Dup %p->%p\n", oldp, newp);

  /* Recover our private data from the old struct file instance */

  oldattr = (FAR struct irq_file_s *)oldp->f_priv;
  DEBUGASSERT(oldattr);

  /* Allocate a new container to hold the task and attribute selection */

  newattr = (FAR struct irq_file_s *)kmm_malloc(sizeof(struct irq_file_s));
  if (!newattr)
    {
      ferr("ERROR: Failed to allocate file attributes\n");
      return -ENOMEM;
    }

  /* The copy the file attributes from the old attributes to the new */

  memcpy(newattr, oldattr, sizeof(struct irq_file_s));

  /* Save the new attributes in the new file structure */

  newp->f_priv = (FAR void *)newattr;
  return OK;
}

/****************************************************************************
 * Name: irq_stat
 *
 * Description: Return information about a file or directory
 *
 ****************************************************************************/

static int irq_stat(const char *relpath, struct stat *buf)
{
  /* "irqs" is the name for a read-only file */

  memset(buf, 0, sizeof(struct stat));
  buf->st_mode = S_IFREG | S_IROTH | S_IRGRP | S_IRUSR;
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

#endif /* CONFIG_SCHED_IRQMONITOR */
#endif /* !CONFIG_DISABLE_MOUNTPOINT && CONFIG_FS_PROCFS */
