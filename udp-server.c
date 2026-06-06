/*
 * BIL 304 - OTA Firmware Alici (udp-server.c)
 * Node 1 calisir: RPL root + UDP server
 *
 * Protokol:
 *   Paket  : [magic(2)][chunk_no(2)][total_chunks(2)][payload_len(1)][checksum8(1)][veri...]
 *   ACK    : [A][C][chunk_no_high][chunk_no_low]   (4 byte binary)
 *   Checksum: payload uzerinde XOR (tum byte'larin XOR'u)
 *
 * Akis:
 *   1. Her blok gelir -> magic/boyut/checksum kontrolu
 *   2. Duplikat ise sadece ACK don
 *   3. Yeni blok ise CFS'e yaz, CRC32 guncelle, ACK gonder
 *   4. Son blok gelince: CRC32 sonuclat, metadata guncelle
 */

#include "contiki.h"
#include "net/routing/routing.h"
#include "net/netstack.h"
#include "net/ipv6/simple-udp.h"
#include "cfs/cfs.h"
#include "cfs/cfs-coffee.h"
#include "ota-metadata.h"
#include "sys/log.h"
#include <stdint.h>
#include <string.h>

#define LOG_MODULE "OTA-Alici"
#define LOG_LEVEL  LOG_LEVEL_INFO

#define UDP_CLIENT_PORT   8765
#define UDP_SERVER_PORT   5678
#define CHUNK_SIZE        48
#define HEADER_LEN        8
#define OTA_MAGIC         0xF17Eu
#define FIRMWARE_FILE     "ota_new.bin"
#define NEW_FW_VERSION    2u

/*---------------------------------------------------------------------------*/
/* Global durum degiskenleri                                                  */
/*---------------------------------------------------------------------------*/
static struct simple_udp_connection udp_conn;

static uint16_t next_expected;        /* sirayla bekledigimiz blok numarasi  */
static uint16_t received_chunks;      /* aldigimiz benzersiz blok sayisi      */
static uint16_t total_chunks_remote;  /* sender'dan ogrendigimiz toplam blok  */
static uint32_t total_fw_size;        /* birikmis firmware byte sayisi        */
static uint8_t  transfer_complete;    /* 1 = transfer bitti                   */

/* Streaming CRC32 durumu - bloklari hafizada biriktirmeden hesapla */
static uint32_t crc_state;

PROCESS(udp_server_process, "UDP server");
AUTOSTART_PROCESSES(&udp_server_process);

/*---------------------------------------------------------------------------*/
/* XOR checksum: payload byte'larinin XOR'u                                  */
/*---------------------------------------------------------------------------*/
static uint8_t
checksum8(const uint8_t *data, uint8_t len)
{
  uint8_t cs = 0;
  uint8_t i;
  for(i = 0; i < len; i++) {
    cs ^= data[i];
  }
  return cs;
}

/*---------------------------------------------------------------------------*/
/* Streaming CRC32 yardimci fonksiyonlari                                    */
/*                                                                            */
/* Neden bu? ota_crc32_buffer() tum bufferi istiyor (129KB RAM'e sigmaz).    */
/* Biz bloklari geldikce CRC'yi guncelliyoruz, son bloktan sonra bitiriyoruz.*/
/* Stop-and-wait ile bloklar sirali geldiginden bu dogru sonucu veriyor.     */
/*---------------------------------------------------------------------------*/
static void
crc32_stream_init(void)
{
  crc_state = 0xFFFFFFFFu;
}

static void
crc32_stream_update(const uint8_t *data, uint8_t len)
{
  /* Polynomial: 0xEDB88320 (IEEE 802.3 - zip/png/ethernet ile ayni) */
  while(len--) {
    uint8_t i;
    crc_state ^= *data++;
    for(i = 0; i < 8; i++) {
      if(crc_state & 1u) {
        crc_state = (crc_state >> 1) ^ 0xEDB88320u;
      } else {
        crc_state >>= 1;
      }
    }
  }
}

static uint32_t
crc32_stream_final(void)
{
  return ~crc_state;
}

/*---------------------------------------------------------------------------*/
/* UDP callback: her UDP paketi geldiginde Contiki tarafindan cagirilir      */
/*---------------------------------------------------------------------------*/
static void
udp_rx_callback(struct simple_udp_connection *c,
                const uip_ipaddr_t *sender_addr,
                uint16_t sender_port,
                const uip_ipaddr_t *receiver_addr,
                uint16_t receiver_port,
                const uint8_t *data,
                uint16_t datalen)
{
  uint16_t magic;
  uint16_t chunk_no;
  uint16_t total_chunks;
  uint8_t  payload_len;
  uint8_t  recv_cs;
  uint8_t  calc_cs;
  uint8_t  ack[4];
  uint32_t offset;
  int      fd;
  int      written;

  /* --- 1. Minimum uzunluk kontrolu --- */
  if(datalen < HEADER_LEN) {
    LOG_INFO("Kisa paket (%u byte), atlandi\n", datalen);
    return;
  }

  /* --- 2. Magic number kontrolu --- */
  magic = ((uint16_t)data[0] << 8) | data[1];
  if(magic != OTA_MAGIC) {
    LOG_INFO("Yanlis magic: 0x%04x, atlandi\n", magic);
    return;
  }

  /* --- 3. Header ayristirma --- */
  chunk_no     = ((uint16_t)data[2] << 8) | data[3];
  total_chunks = ((uint16_t)data[4] << 8) | data[5];
  payload_len  = data[6];
  recv_cs      = data[7];

  /* --- 4. Paket boyutu tutarlilik kontrolu --- */
  if(datalen != (uint16_t)(HEADER_LEN + payload_len)) {
    LOG_INFO("Boyut uyumsuzlugu: datalen=%u beklenen=%u (blok %u)\n",
             datalen, HEADER_LEN + payload_len, chunk_no);
    return;
  }

  /* --- 5. XOR checksum dogrulama --- */
  calc_cs = checksum8(&data[HEADER_LEN], payload_len);
  if(calc_cs != recv_cs) {
    LOG_INFO("Checksum HATASI! Blok %u: hesaplanan=0x%02x gelen=0x%02x\n",
             chunk_no, calc_cs, recv_cs);
    /* ACK gonderme: sender timeout'ta tekrar gonderecek */
    return;
  }

  /* --- 6. Transfer zaten bittiyse sadece ACK gonder --- */
  if(transfer_complete) {
    ack[0] = 'A'; ack[1] = 'C';
    ack[2] = (uint8_t)(chunk_no >> 8);
    ack[3] = (uint8_t)(chunk_no);
    simple_udp_sendto(&udp_conn, ack, 4, sender_addr);
    return;
  }

  /* --- 7. Duplikat blok kontrolu ---
   *
   * Stop-and-wait'te: sender blok N'i gonderir, ACK N bekler.
   * ACK kaybolursa: sender 5sn sonra blok N'i TEKRAR gonderir.
   * Biz zaten ilerlediysek (next_expected > N):
   *   - CFS'e tekrar yazma (gereksiz)
   *   - CRC'yi guncelleme (bozulur!)
   *   - Sadece ACK gonder ki sender devam etsin
   */
  if(chunk_no < next_expected) {
    LOG_INFO("Duplikat blok %u (beklenen=%u), ACK tekrarlandi\n",
             chunk_no, next_expected);
    ack[0] = 'A'; ack[1] = 'C';
    ack[2] = (uint8_t)(chunk_no >> 8);
    ack[3] = (uint8_t)(chunk_no);
    simple_udp_sendto(&udp_conn, ack, 4, sender_addr);
    return;
  }

  /* --- 8. Siradisi blok (stop-and-wait'te olmamali, savunmaci kod) --- */
  if(chunk_no != next_expected) {
    LOG_INFO("Siradisi blok! beklenen=%u gelen=%u, atlandi\n",
             next_expected, chunk_no);
    return;
  }

  /* --- 9. CFS'e yaz ---
   *
   * Coffee File System: Z1'deki harici flash bellek (M25P16, 2MB).
   * Blok N -> offset = N * 48 konumuna yaziyoruz.
   * Boylece tam firmware CFS dosyasinda olusturuluyor.
   */
  offset = (uint32_t)chunk_no * CHUNK_SIZE;
  fd = cfs_open(FIRMWARE_FILE, CFS_WRITE);
  if(fd < 0) {
    LOG_INFO("CFS acma HATASI! Blok %u kaydedilemedi\n", chunk_no);
    return;
  }
  if(cfs_seek(fd, (cfs_offset_t)offset, CFS_SEEK_SET) < 0) {
    LOG_INFO("CFS seek HATASI! offset=%lu\n", (unsigned long)offset);
    cfs_close(fd);
    return;
  }
  written = cfs_write(fd, &data[HEADER_LEN], payload_len);
  cfs_close(fd);

  if(written != (int)payload_len) {
    LOG_INFO("CFS yazma HATASI! %d/%u byte (blok %u)\n",
             written, payload_len, chunk_no);
    return;
  }

  /* --- 10. Streaming CRC32 guncelle (sirali bloklarla dogru calisir) --- */
  crc32_stream_update(&data[HEADER_LEN], payload_len);

  /* --- 11. Sayaclari guncelle --- */
  received_chunks++;
  next_expected++;
  total_fw_size += payload_len;
  total_chunks_remote = total_chunks;

  LOG_INFO("Blok %u/%u | offset=%lu | %u byte | cs=OK | toplam=%u\n",
           chunk_no + 1, total_chunks,
           (unsigned long)offset, payload_len, received_chunks);

  /* --- 12. ACK gonder --- */
  ack[0] = 'A'; ack[1] = 'C';
  ack[2] = (uint8_t)(chunk_no >> 8);
  ack[3] = (uint8_t)(chunk_no);
  simple_udp_sendto(&udp_conn, ack, 4, sender_addr);

  /* --- 13. Tum bloklar alindiysa process'i uyandır --- */
  if(received_chunks >= total_chunks) {
    transfer_complete = 1;
    process_poll(&udp_server_process);
  }
}

/*---------------------------------------------------------------------------*/
PROCESS_THREAD(udp_server_process, ev, data)
{
  static ota_boot_metadata_t metadata;
  uint32_t fw_crc;

  PROCESS_BEGIN();

  /* RPL DAG root baslat: bu dugum ag agacinin kokudur */
  NETSTACK_ROUTING.root_start();

  /* Streaming CRC'yi sifirla */
  crc32_stream_init();

  /* UDP baglantisini kaydet */
  simple_udp_register(&udp_conn, UDP_SERVER_PORT, NULL,
                      UDP_CLIENT_PORT, udp_rx_callback);

  LOG_INFO("=== OTA Alici Hazir ===\n");
  LOG_INFO("Port %u uzerinde dinleniyor...\n", UDP_SERVER_PORT);

  /* Transfer tamamlanana kadar bekle
   * process_poll() callback'ten cagirilinca bu kosul true olacak */
  PROCESS_WAIT_EVENT_UNTIL(transfer_complete);

  /* --- Transfer tamamlandi --- */
  fw_crc = crc32_stream_final();

  LOG_INFO("=== %u/%u BLOK ALINDI ===\n", received_chunks, total_chunks_remote);
  LOG_INFO("Firmware boyutu: %lu byte\n", (unsigned long)total_fw_size);
  LOG_INFO("Hesaplanan CRC32: 0x%08lx\n", (unsigned long)fw_crc);
  LOG_INFO("Yuklenmeye hazir yeni firmware alimi tamamlandi!\n");

  /* --- OTA Metadata guncelle ---
   *
   * Dual-slot OTA sistemi:
   *   Slot A = su an calisan firmware (mevcut)
   *   Slot B = yeni indirdigimiz firmware
   *
   * mark_verified: boyut ve CRC'yi kaydet, Slot B durumunu VERIFIED yap
   * stage_verified: Slot B durumunu PENDING yap (bir sonraki boot'ta aktif olur)
   */
  LOG_INFO("OTA metadata guncelleniyor...\n");

  memset(&metadata, 0, sizeof(metadata));
  metadata.magic       = OTA_IMAGE_MAGIC;
  metadata.active_slot = OTA_SLOT_A;

  if(ota_metadata_mark_verified(&metadata, OTA_SLOT_B,
                                 NEW_FW_VERSION,
                                 total_fw_size,
                                 fw_crc)) {
    LOG_INFO("Slot B VERIFIED: v%u boyut=%lu crc=0x%08lx\n",
             NEW_FW_VERSION,
             (unsigned long)total_fw_size,
             (unsigned long)fw_crc);
  } else {
    LOG_INFO("Slot B isaretleme HATASI!\n");
  }

  if(ota_metadata_stage_verified_image(&metadata, OTA_SLOT_B)) {
    LOG_INFO("Slot B PENDING: sonraki acilista yeni firmware aktif olacak.\n");
  } else {
    LOG_INFO("Slot B staging HATASI!\n");
  }

  LOG_INFO("=== OTA TRANSFERI TAMAMLANDI ===\n");

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
