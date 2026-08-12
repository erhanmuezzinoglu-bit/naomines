#ifndef TRACE_H
#define TRACE_H

#include <stdint.h>
#include "fake6502.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 
 * NES emülasyonunda karşılaşılabilecek kritik durumlar.
 * "Halt" yerine "Alert" mantığıyla çalışırlar.
 */
typedef enum halt_reason
{
    HALT_NONE = 0,
    HALT_UNKNOWN_IO_READ,   /* Tanımlanmamış adresten okuma */
    HALT_UNKNOWN_IO_WRITE,  /* Tanımlanmamış adrese yazma */
    HALT_ILLEGAL_OPCODE,    /* Resmi olmayan/hatalı işlem kodu */
    HALT_STACK_OVERFLOW,    /* Stack pointer sınırı aşıldı */
    HALT_PPU_SYNC_ERROR,    /* PPU ve CPU senkronizasyonu koptu */
    HALT_SPRITE_0_TIMEOUT   /* Sprite 0 Hit beklenirken zaman aşımı oluştu */
} halt_reason_t;

typedef struct trace_evt
{
    uint16_t pc;        /* İşlem anındaki Program Counter */
    uint16_t addr;      /* Erişim sağlanan bellek adresi */
    uint8_t  val;       /* Okunan/Yazılan değer */
    uint8_t  is_write;  /* 0=read, 1=write */
    uint8_t  unknown;   /* Bilinmeyen IO erişimi bayrağı */
    uint8_t  opcode;    /* İşlenen komutun ham kodu (Diagnostic için) */

    /* CPU Register Snapshotları (Nestest uyumluluğu ve hata takibi için) */
    uint8_t  a;
    uint8_t  x;
    uint8_t  y;
    uint8_t  p;
    uint8_t  sp;
} trace_evt_t;

/* 
 * Ring Buffer boyutu. 256 işlem genellikle yeterlidir.
 */
#define TRACE_SIZE 256

typedef struct trace_state
{
    trace_evt_t rb[TRACE_SIZE];
    uint32_t head;      /* Yazılacak sonraki index */
    int enable;         /* Trace kaydı aktif mi? */

    /* Gözlemci Modu Bilgileri: Akışı durdurmaz, sadece işaretler */
    int error_detected;     /* Hata saptandı mı? */
    halt_reason_t halt_reason;
    uint16_t halt_pc;
    uint16_t halt_addr;
    uint8_t halt_val;
} trace_state_t;

/* --- Fonksiyon Protokolleri --- */

/**
 * Trace sistemini sıfırlar ve başlatır.
 */
void trace_init(trace_state_t *t);

/**
 * Mevcut CPU ve Bus durumunu ring buffer'a kaydeder.
 * Sistem akışını asla etkilemez.
 */
void trace_push(trace_state_t *t, fake6502_context *ctx,
                uint16_t addr, uint8_t val, int is_write, int unknown);

/**
 * Hata durumunu kaydeder ancak CPU'yu dondurmaz. 
 * Accuracy testlerinin devam etmesini sağlar.
 */
void trace_halt_now(trace_state_t *t, fake6502_context *ctx,
                    halt_reason_t r, uint16_t addr, uint8_t val);

/**
 * Durumu okunabilir metne dönüştürür.
 */
const char *trace_halt_reason_str(halt_reason_t r);

#ifdef __cplusplus
}
#endif

#endif /* TRACE_H */