// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
  uint64 nfree; // páginas en freelist
} kmem;

// Conteo de referencias por página física (KERNBASE..PHYSTOP).
// Lo protege su propio spinlock, distinto de kmem.lock. Regla: nunca se
// tiene reflock mientras se toma kmem.lock (ni al revés), así no hay
// deadlock entre kalloc() y kfree().
struct {
  struct spinlock lock;
  uchar count[(PHYSTOP - KERNBASE) / PGSIZE];
} kref;

#define REFIDX(pa) (((uint64)(pa) - KERNBASE) / PGSIZE)

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  initlock(&kref.lock, "kref");
  freerange(end, (void *)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char *)PGROUNDUP((uint64)pa_start);
  for (; p + PGSIZE <= (char *)pa_end; p += PGSIZE) {
    // estas páginas nunca pasaron por kalloc(): kfree() las espera en 1
    kref.count[REFIDX(p)] = 1;
    kfree(p);
  }
}

// Add a reference to the page at pa (it is now mapped one more time).
void
krefinc(void *pa)
{
  if (((uint64)pa % PGSIZE) != 0 || (char *)pa < end || (uint64)pa >= PHYSTOP)
    panic("krefinc");
  acquire(&kref.lock);
  if (kref.count[REFIDX(pa)] < 1)
    panic("krefinc: sin referencias");
  kref.count[REFIDX(pa)]++;
  release(&kref.lock);
}

// Number of references to the page at pa.
int
krefcount(void *pa)
{
  int n;

  acquire(&kref.lock);
  n = kref.count[REFIDX(pa)];
  release(&kref.lock);
  return n;
}

// Number of physical pages in the free list.
uint64
kfreepages(void)
{
  uint64 n;

  acquire(&kmem.lock);
  n = kmem.nfree;
  release(&kmem.lock);
  return n;
}

// Number of physical pages with more than one reference.
uint64
ksharedpages(void)
{
  uint64 n = 0;

  acquire(&kref.lock);
  for (uint64 i = 0; i < sizeof(kref.count); i++)
    if (kref.count[i] > 1)
      n++;
  release(&kref.lock);
  return n;
}

// Drop one reference to the page of physical memory pointed at by pa;
// it returns to the free list only when the last reference is gone.
// pa normally should have been returned by a call to kalloc().
// (The exception is when initializing the allocator; see freerange.)
void
kfree(void *pa)
{
  struct run *r;

  if (((uint64)pa % PGSIZE) != 0 || (char *)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  acquire(&kref.lock);
  if (kref.count[REFIDX(pa)] < 1)
    panic("kfree: sin referencias");
  if (--kref.count[REFIDX(pa)] > 0) {
    release(&kref.lock);
    return;
  }
  release(&kref.lock);

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run *)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  kmem.nfree++;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if (r) {
    kmem.freelist = r->next;
    kmem.nfree--;
  }
  release(&kmem.lock);

  if (r) {
    memset((char *)r, 5, PGSIZE); // fill with junk
    acquire(&kref.lock);
    kref.count[REFIDX(r)] = 1;
    release(&kref.lock);
  }
  return (void *)r;
}
