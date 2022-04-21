struct buf {
  int valid;   // has data been read from disk?
  int disk;    // does disk "own" buf?
  uint dev;
  uint blockno;
  struct sleeplock lock;
  uint refcnt;
  uint timestamp;
  struct buf *next;  // hash table next
  uchar data[BSIZE];
};

