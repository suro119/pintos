void cache_read_at (block_sector_t, void *, int, int);
void cache_write_at (block_sector_t, const void *, int, int);
void cache_init (void);
void cache_done (void);
void cache_remove (block_sector_t);
