#ifndef _U8_MODULE
#define _U8_MODULE
 
void do_U8_archive(u8 *buffer, char *path);
u32 do_file_U8_archive(u8 *buffer, char *filename, u8 **data_out);
u16 be16(const u8 *p);
u32 be32(const u8 *p);

 
#endif

