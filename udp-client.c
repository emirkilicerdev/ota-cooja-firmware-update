#include "contiki.h"
#include "net/routing/routing.h"
#include "net/netstack.h"
#include "net/ipv6/simple-udp.h"
#include "sys/node-id.h"
#include "sys/log.h"
#include "firmware_data.h"
#include <stdint.h>
#include <string.h>

#define LOG_MODULE "OTA-Sender"
#define LOG_LEVEL  LOG_LEVEL_INFO

/* ── Port numaralari ── */
#define UDP_CLIENT_PORT  8765
#define UDP_SERVER_PORT  5678

/* ── OTA protokol sabitleri ── */
#define CHUNK_SIZE    48          /* Her blok kac byte? */
#define HEADER_LEN    8           /* Paket basliginin boyutu */
#define OTA_MAGIC     0xF17Eu    /* "Bu bir OTA paketi" isaretcisi */
#define TIMEOUT_SEC   5           /* ACK icin bekleme suresi (saniye) */
#define MAX_RETRIES   5           /* Max kac kez tekrar gonderelim? */

/* FINAL_MARKER: ozel blok numarasi. chunk_no = 0xFFFF gelirse bu normal bir
 * veri blogu degil, "tum-imaj CRC dogrulama" paketidir. Gercek blok no'lar
 * 0-99 oldugundan 0xFFFF asla cakismaz. */
#define FINAL_MARKER  0xFFFFu

/* Toplam blok sayisi: 4800 / 48 = 100 blok */
#define TOTAL_CHUNKS  ((uint16_t)((FIRMWARE_PAYLOAD_LEN + CHUNK_SIZE - 1u) / CHUNK_SIZE))

/*---------------------------------------------------------------------------*/
/* DURUM DEGISKENLERI - "static" olmak zorunda!
 * Neden? Protothread'ler PROCESS_WAIT sirasinda yerel degiskenleri kaybeder.
 * static dersek, deger bekleme boyunca korunur.
 */
static struct simple_udp_connection udp_conn;
static uint16_t current_chunk;   /* Simdi hangi bloku gonderiyoruz? */
static uint8_t  waiting_ack;     /* ACK bekleniyor mu? 1=evet, 0=hayir */
static uint8_t  retries;         /* Kac kez tekrar denedik? */
static uint8_t  sending_final;   /* 1 = artik FINAL paketi gonderiyoruz */
static uint8_t  final_acked;     /* 1 = alici CRC'yi onayladi, is bitti */
static uint8_t  need_restart;    /* 1 = NACK geldi, blok 0'dan basla */
static uint32_t firmware_crc;    /* tum firmware'in CRC32'si (bir kez hesaplanir) */

/* Surecimizi tanimliyoruz */
PROCESS(ota_sender_process, "OTA Sender");
AUTOSTART_PROCESSES(&ota_sender_process);

/*---------------------------------------------------------------------------*/
/* YARDIMCI FONKSIYON: Basit XOR checksum
 * Payload'daki tum byte'lari XOR'larız.
 * Alici ayni islemi yapar; farkli cikiyorsa paket bozuktur.
 */
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
/* YARDIMCI FONKSIYON: Bir blok olustur ve gonder */
static void
send_chunk(const uip_ipaddr_t *dest, uint16_t chunk_no)
{
  static uint8_t packet[HEADER_LEN + CHUNK_SIZE];
  uint32_t offset;
  uint8_t  payload_len;
  uint8_t  cs;

  /* Bu blogun firmware'deki baslangic yeri */
  offset = (uint32_t)chunk_no * CHUNK_SIZE;

  /* Son blok kismi olabilir (129760 / 48 tam bolunmuyor) */
  if(offset + CHUNK_SIZE > FIRMWARE_PAYLOAD_LEN) {
    payload_len = (uint8_t)(FIRMWARE_PAYLOAD_LEN - offset);
  } else {
    payload_len = CHUNK_SIZE;
  }

  /* Firmware verisini pakete kopyala */
  memcpy(&packet[HEADER_LEN], &firmware_payload[offset], payload_len);

  /* Checksum hesapla (sadece payload uzerinden) */
  cs = checksum8(&packet[HEADER_LEN], payload_len);

  /* ── PAKET BASLIGI (8 byte) ──────────────────────────
   *  [0-1] Magic    : 0xF17E  → "Bu OTA paketidir"
   *  [2-3] Blok no  : 0-2703  → "Kacinci blok bu?"
   *  [4-5] Toplam   : 2704    → "Toplam kac blok var?"
   *  [6]   Boyut    : 1-48    → "Bu blokta kac byte var?"
   *  [7]   Checksum : XOR     → "Hata kontrolu"
   */
  packet[0] = (uint8_t)(OTA_MAGIC >> 8);
  packet[1] = (uint8_t)(OTA_MAGIC);
  packet[2] = (uint8_t)(chunk_no >> 8);
  packet[3] = (uint8_t)(chunk_no);
  packet[4] = (uint8_t)(TOTAL_CHUNKS >> 8);
  packet[5] = (uint8_t)(TOTAL_CHUNKS);
  packet[6] = payload_len;
  packet[7] = cs;

  LOG_INFO("Blok %u/%u gonderildi | offset=%u | %u byte | cs=0x%02x\n",
           (unsigned)(chunk_no + 1), (unsigned)TOTAL_CHUNKS,
           (unsigned)offset, (unsigned)payload_len, (unsigned)cs);

  simple_udp_sendto(&udp_conn, packet, HEADER_LEN + payload_len, dest);
}

/*---------------------------------------------------------------------------*/
/* YARDIMCI FONKSIYON: Tum firmware'in CRC32'sini hesapla
 *
 * Gonderici, gondereceği firmware'in dogru CRC'sini onceden bilmeli ki
 * aliciya "beklenen deger bu" diye yollasin. firmware_payload flash'ta
 * (rodata) durdugundan tek tek okuyup hesapliyoruz - RAM'e yuklemeye gerek yok.
 * Alici da ayni byte'lar uzerinden ayni sirayla hesapladigi icin sonuc ESIT olmali.
 */
static uint32_t
compute_firmware_crc(void)
{
  uint32_t crc = 0xFFFFFFFFu;
  uint32_t n;
  for(n = 0; n < FIRMWARE_PAYLOAD_LEN; n++) {
    uint8_t i;
    crc ^= firmware_payload[n];
    for(i = 0; i < 8; i++) {
      if(crc & 1u) {
        crc = (crc >> 1) ^ 0xEDB88320u;
      } else {
        crc >>= 1;
      }
    }
  }
  return ~crc;
}

/*---------------------------------------------------------------------------*/
/* YARDIMCI FONKSIYON: FINAL paketi gonder (tum-imaj CRC dogrulamasi)
 *
 * Tum bloklar gidince, beklenen CRC'yi bu ozel pakette yolluyoruz.
 * Paket yapisi normal blokla ayni ama:
 *   - chunk_no = 0xFFFF (FINAL_MARKER) -> "bu CRC paketi"
 *   - payload  = 4 byte CRC32 (big-endian)
 */
static void
send_final(const uip_ipaddr_t *dest, uint32_t crc)
{
  static uint8_t packet[HEADER_LEN + 4];
  uint8_t cs;

  /* Payload = 4 byte CRC32 (yuksek byte once) */
  packet[HEADER_LEN + 0] = (uint8_t)(crc >> 24);
  packet[HEADER_LEN + 1] = (uint8_t)(crc >> 16);
  packet[HEADER_LEN + 2] = (uint8_t)(crc >> 8);
  packet[HEADER_LEN + 3] = (uint8_t)(crc);

  cs = checksum8(&packet[HEADER_LEN], 4);

  packet[0] = (uint8_t)(OTA_MAGIC >> 8);
  packet[1] = (uint8_t)(OTA_MAGIC);
  packet[2] = (uint8_t)(FINAL_MARKER >> 8);   /* 0xFF */
  packet[3] = (uint8_t)(FINAL_MARKER);        /* 0xFF */
  packet[4] = (uint8_t)(TOTAL_CHUNKS >> 8);
  packet[5] = (uint8_t)(TOTAL_CHUNKS);
  packet[6] = 4;                               /* payload_len = 4 */
  packet[7] = cs;

  LOG_INFO("FINAL paketi gonderildi | beklenen CRC32=0x%08lx\n",
           (unsigned long)crc);

  simple_udp_sendto(&udp_conn, packet, HEADER_LEN + 4, dest);
}

/*---------------------------------------------------------------------------*/
/* CALLBACK: Alicidan ACK geldiginde bu fonksiyon cagrilir
 * Bu bir protothread degil, normal C fonksiyonu.
 * Dolayisiyla PROCESS_WAIT gibi komutlar kullanılamaz.
 */
static void
udp_rx_callback(struct simple_udp_connection *c,
                const uip_ipaddr_t *sender_addr,
                uint16_t sender_port,
                const uip_ipaddr_t *receiver_addr,
                uint16_t receiver_port,
                const uint8_t *data,
                uint16_t datalen)
{
  uint16_t acked_chunk;

  if(datalen < 4) {
    return; /* cok kisa, OTA cevabi degil */
  }

  /* ── NACK paketi: [0]='N' [1]='K' → "CRC tutmadi, BASTAN gonder" ── */
  if(data[0] == 'N' && data[1] == 'K') {
    LOG_INFO("NACK alindi: CRC tutmadi, transfer BASTAN baslayacak!\n");
    need_restart = 1;
    sending_final = 0;
    waiting_ack = 0;
    retries = 0;
    process_poll(&ota_sender_process);
    return;
  }

  /* ── ACK paketi: [0]='A' [1]='C' [2]=chunk_yuksek [3]=chunk_dusuk ── */
  if(data[0] != 'A' || data[1] != 'C') {
    return; /* OTA cevabi degil, yoksay */
  }

  acked_chunk = ((uint16_t)data[2] << 8) | data[3];

  /* ── FINAL ACK: alici CRC'yi onayladi (chunk 0xFFFF) ── */
  if(acked_chunk == FINAL_MARKER) {
    LOG_INFO("FINAL ACK alindi: CRC dogrulandi, transfer TAMAM!\n");
    final_acked = 1;
    waiting_ack = 0;
    process_poll(&ota_sender_process);
    return;
  }

  /* ── Normal blok ACK ── */
  if(acked_chunk == current_chunk) {
    LOG_INFO("ACK alindi: Blok %u onaylandi\n", acked_chunk);
    waiting_ack = 0;  /* ACK geldi! */
    retries     = 0;
    current_chunk++;

    /* Surecimizi uyandır: "ACK geldi, devam et!" */
    process_poll(&ota_sender_process);
  }
}

/*---------------------------------------------------------------------------*/
/* ANA SUREC: OTA gonderici dongusu
 *
 * ONEMLI: PROCESS_END() sadece en sonda, bir kez olmali!
 * Eger "if(kosul) { PROCESS_END(); }" yapilirsa,
 * PROCESS_END icindeki "}" PROCESS_BEGIN'in actigi scope'u erkenden kapatir,
 * sonraki PROCESS_WAIT satirlari switch-case disinda kalir -> derleme hatasi!
 *
 * Dogru yaklasim: if/else ile saran yapi, PROCESS_END en sonda.
 */
PROCESS_THREAD(ota_sender_process, ev, data)
{
  static struct etimer   timer;
  static uip_ipaddr_t    dest_ipaddr;  /* static: PROCESS_WAIT'te kaybolmasin */

  PROCESS_BEGIN();

  /* UDP baglantisini kaydet (her dugum icin) */
  simple_udp_register(&udp_conn, UDP_CLIENT_PORT, NULL,
                      UDP_SERVER_PORT, udp_rx_callback);

  if(node_id == 2) {
    /* ── Sadece Dugum 2 gonderici rolundedir ── */

    /* Tum firmware'in CRC32'sini bir kez hesapla (aliciya yollayacagiz) */
    firmware_crc = compute_firmware_crc();

    LOG_INFO("=== OTA Sender: %u byte, %u blok, CRC32=0x%08lx ===\n",
             (unsigned)FIRMWARE_PAYLOAD_LEN, (unsigned)TOTAL_CHUNKS,
             (unsigned long)firmware_crc);

    /* RPL aginin kurulmasi icin bekle */
    LOG_INFO("Ag kurulumu bekleniyor (10 sn)...\n");
    etimer_set(&timer, 10 * CLOCK_SECOND);
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&timer));

    /* ── ANA GONDERIM DONGUSU ──
     * Iki faz var:
     *   1) BLOK FAZI : 0..99 bloklarini stop-and-wait ile gonder
     *   2) FINAL FAZI: tum bloklar gidince beklenen CRC'yi yolla
     * Alici CRC'yi onaylayinca (final_acked) is biter.
     * Alici NACK yollarsa (need_restart) blok 0'dan yeniden baslariz.
     */
    while(!final_acked) {

      /* NACK geldiyse: durumu sifirla, blok 0'dan basla */
      if(need_restart) {
        LOG_INFO("=== BASTAN BASLANIYOR (CRC tutmadi) ===\n");
        current_chunk = 0;
        sending_final = 0;
        waiting_ack = 0;
        retries = 0;
        need_restart = 0;
      }

      /* Ag hazir mi? */
      if(!NETSTACK_ROUTING.node_is_reachable() ||
         !NETSTACK_ROUTING.get_root_ipaddr(&dest_ipaddr)) {
        LOG_INFO("Ag hazir degil, 2 sn bekleniyor...\n");
        etimer_set(&timer, 2 * CLOCK_SECOND);
        PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&timer));
        continue;
      }

      if(current_chunk < TOTAL_CHUNKS) {
        /* ════════ BLOK FAZI ════════ */

        /* Timeout kontrolu: onceki bloga ACK gelmedi mi? */
        if(waiting_ack) {
          retries++;
          if(retries > MAX_RETRIES) {
            LOG_INFO("HATA: Blok %u icin %u deneme, atlaniyor!\n",
                     (unsigned)current_chunk, (unsigned)MAX_RETRIES);
            current_chunk++;
            waiting_ack = 0;
            retries = 0;
            continue;
          }
          LOG_INFO("Timeout! Blok %u tekrar (%u/%u)...\n",
                   (unsigned)current_chunk, (unsigned)retries, (unsigned)MAX_RETRIES);
        }

        send_chunk(&dest_ipaddr, current_chunk);
        waiting_ack = 1;

      } else {
        /* ════════ FINAL FAZI (tum bloklar gitti) ════════ */

        if(!sending_final) {
          LOG_INFO("=== TUM %u BLOK GONDERILDI, CRC dogrulamasi yollaniyor ===\n",
                   (unsigned)TOTAL_CHUNKS);
          sending_final = 1;
        }

        /* FINAL'e cevap gelmediyse tekrar yolla */
        if(waiting_ack) {
          retries++;
          if(retries > MAX_RETRIES) {
            LOG_INFO("HATA: FINAL icin cevap yok, vazgeciliyor.\n");
            break;
          }
          LOG_INFO("Timeout! FINAL tekrar (%u/%u)...\n",
                   (unsigned)retries, (unsigned)MAX_RETRIES);
        }

        send_final(&dest_ipaddr, firmware_crc);
        waiting_ack = 1;
      }

      /* Bekleme: timer DOLARSA timeout, process_poll gelirse cevap var */
      etimer_set(&timer, TIMEOUT_SEC * CLOCK_SECOND);
      PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&timer) || !waiting_ack || need_restart);
    }

    if(final_acked) {
      LOG_INFO("=== OTA TRANSFERI BASARIYLA TAMAMLANDI! ===\n");
    }

  } else {
    /* Dugum 3: sadece RPL relay, gonderici degil */
    LOG_INFO("Dugum %u: relay modunda, OTA gonderici degil.\n", (unsigned)node_id);
  }

  PROCESS_END();
}
