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
static uint8_t  transfer_done;   /* Transfer tamamlandi mi? */

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

  /* ACK paketi: [0]='A' [1]='C' [2]=chunk_yuksek [3]=chunk_dusuk */
  if(datalen < 4 || data[0] != 'A' || data[1] != 'C') {
    return; /* OTA ACK degil, yoksay */
  }

  acked_chunk = ((uint16_t)data[2] << 8) | data[3];

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
    LOG_INFO("=== OTA Sender: %u byte, %u blok ===\n",
             (unsigned)FIRMWARE_PAYLOAD_LEN, (unsigned)TOTAL_CHUNKS);

    /* RPL aginin kurulmasi icin bekle */
    LOG_INFO("Ag kurulumu bekleniyor (10 sn)...\n");
    etimer_set(&timer, 10 * CLOCK_SECOND);
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&timer));

    /* ── ANA GONDERIM DONGUSU ── */
    while(!transfer_done) {

      /* Hepsi gonderildi mi? */
      if(current_chunk >= TOTAL_CHUNKS) {
        LOG_INFO("=== TUM %u BLOK GONDERILDI! ===\n", (unsigned)TOTAL_CHUNKS);
        transfer_done = 1;
        break;
      }

      /* Ag hazir mi? */
      if(!NETSTACK_ROUTING.node_is_reachable() ||
         !NETSTACK_ROUTING.get_root_ipaddr(&dest_ipaddr)) {
        LOG_INFO("Ag hazir degil, 2 sn bekleniyor...\n");
        etimer_set(&timer, 2 * CLOCK_SECOND);
        PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&timer));
        continue;
      }

      /* Timeout kontrolu: onceki bloga ACK gelmedi mi? */
      if(waiting_ack) {
        retries++;
        if(retries > MAX_RETRIES) {
          LOG_INFO("HATA: Blok %u icin %u deneme yapildi, atlaniyor!\n",
                   (unsigned)current_chunk, (unsigned)MAX_RETRIES);
          current_chunk++;
          waiting_ack = 0;
          retries = 0;
          continue;
        }
        LOG_INFO("Timeout! Blok %u tekrar (%u/%u)...\n",
                 (unsigned)current_chunk, (unsigned)retries, (unsigned)MAX_RETRIES);
      }

      /* Blogu gonder ve ACK bekle */
      send_chunk(&dest_ipaddr, current_chunk);
      waiting_ack = 1;

      /* Bekleme: timer DOLARSA timeout, process_poll gelirse ACK var */
      etimer_set(&timer, TIMEOUT_SEC * CLOCK_SECOND);
      PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&timer) || !waiting_ack);
    }

    LOG_INFO("=== OTA TRANSFERI TAMAMLANDI! ===\n");

  } else {
    /* Dugum 3: sadece RPL relay, gonderici degil */
    LOG_INFO("Dugum %u: relay modunda, OTA gonderici degil.\n", (unsigned)node_id);
  }

  PROCESS_END();
}
