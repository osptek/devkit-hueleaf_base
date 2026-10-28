#ifndef QRCODE_H
#define QRCODE_H

#include <stddef.h>
#include <stdint.h>

#define QR_MAX_VERSION  4
#define QR_MAX_SIZE     33

/* modules: size*size 字节，1=黑 0=白。成功返回模块边长，失败返回 0 */
int qrcode_encode(const char *text, uint8_t *modules, int modules_cap, int *out_size);

#endif /* QRCODE_H */
