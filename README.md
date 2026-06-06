# BIL 304 — Contiki-NG OTA Firmware Güncelleme Sistemi (Bölüm 1: Geliştirme Süreci)

> Ondokuz Mayıs Üniversitesi — Bilgisayar Mühendisliği — BIL 304 İşletim Sistemleri
> Dönem Çalışması: *CC1352R Platformlarında Contiki-NG Telsiz Donanım Yazılımının Güncellemesi*

Bu proje, Cooja simülatöründe Contiki-NG işletim sistemi üzerinde **OTA (Over-The-Air /
Havadan Güncelleme)** prensibiyle bir firmware (bellenim) dosyasının düğümler arasında
**parçalı, doğrulamalı ve yeniden iletimli** olarak gönderilmesini gerçekler.

## 🎥 Demo Videosu

**YouTube:** `BURAYA_YOUTUBE_LINKI_GELECEK`

> Video, Cooja ortamında sistemin çalışmasını, kod parçalarını ve kullanılan CRC32 hash
> algoritmasının teorisini içermektedir.

---

## 📐 Sistem Mimarisi

Simülasyon (`BIL304-OS-Project-1.csc`) **3 adet Z1 mote** içerir:

```
   ┌──────────┐   new-firmware    ┌──────────┐    iletim     ┌──────────┐
   │ Düğüm 2  │ ───── blok ─────▶ │ Düğüm 3  │ ───────────▶ │ Düğüm 1  │
   │ GÖNDERİCİ│ ◀──── ACK ─────── │  RELAY   │ ◀─────────── │  ALICI   │
   │(client)  │                   │ (client) │               │(server)  │
   └──────────┘                   └──────────┘               └──────────┘
   udp-client.c                   udp-client.c               udp-server.c
```

| Düğüm | Rol | Dosya | Görev |
|-------|-----|-------|-------|
| **1** | OTA Alıcı + RPL Kök (DAG root) | `udp-server.c` | Blokları alır, doğrular, CFS'e yazar, ACK gönderir |
| **2** | OTA Gönderici | `udp-client.c` | Firmware'i parçalayıp gönderir, ACK bekler |
| **3** | Relay (İletici komşu) | `udp-client.c` | Sadece RPL ile paketleri iletir, OTA mantığı çalışmaz |

**Neden relay gerekli?** UDGM radyo modelinde iletim menzili **50 birim**. Düğüm 2 → Düğüm 1
mesafesi **63 birim > 50** olduğundan doğrudan iletişim kurulamaz; Düğüm 3 ortada durup
RPL yönlendirmesiyle paketleri iletir.

**Neden tek kaynak iki rol (Düğüm 2 ve 3)?** Her iki düğüme de `udp-client.z1` yüklenir;
kod içinde `node_id` kontrolü ile ayrışırlar — Düğüm 2 gönderici olur, Düğüm 3 relay
modunda kalır. Bu, `node-id` kütüphanesi ve `if` blokları sayesinde **tek firmware ile
iki davranış** elde etme yöntemidir.

---

## 📦 Paket Formatı ve Uzunlukları

### Veri Paketi (Gönderici → Alıcı)

8 byte sabit başlık + değişken uzunlukta yük (payload):

```
┌─────────┬─────────┬──────────────┬──────────┬──────────┬─────────────────┐
│ Magic   │ Blok No │ Toplam Blok  │ Yük Boyu  │ Checksum │   Yük (payload)  │
│ 2 byte  │ 2 byte  │   2 byte     │  1 byte   │  1 byte  │   1–48 byte      │
└─────────┴─────────┴──────────────┴──────────┴──────────┴─────────────────┘
[0-1]     [2-3]      [4-5]          [6]         [7]        [8 ...]
0xF17E    0–99       100            48 (son:32) XOR
```

| Alan | Offset | Uzunluk | Açıklama |
|------|--------|---------|----------|
| Magic | 0–1 | **2 byte** | `0xF17E` — "bu bir OTA paketidir" işareti |
| Blok No | 2–3 | **2 byte** | Kaçıncı blok (0'dan başlar) |
| Toplam Blok | 4–5 | **2 byte** | Toplam blok sayısı (100) |
| Yük Boyu | 6 | **1 byte** | Bu blokta kaç byte yük var (1–48) |
| Checksum | 7 | **1 byte** | Yük üzerinde XOR (hata kontrolü) |
| Yük | 8+ | **1–48 byte** | Firmware verisinin bu parçası |

- **Başlık uzunluğu:** `HEADER_LEN = 8 byte` (sabit)
- **Blok boyutu:** `CHUNK_SIZE = 48 byte` (6LoWPAN/802.15.4 tek paketine sığacak şekilde)
- **Maksimum paket:** `8 + 48 = 56 byte` — IEEE 802.15.4'ün 127 byte MTU'suna rahatça sığar

### ACK Paketi (Alıcı → Gönderici)

```
┌──────┬──────┬──────────────┬──────────────┐
│ 'A'  │ 'C'  │ Blok No (üst) │ Blok No (alt) │
│ 0x41 │ 0x43 │   1 byte      │   1 byte      │
└──────┴──────┴──────────────┴──────────────┘
```

- **ACK uzunluğu:** **4 byte** (sabit, ikili/binary format)
- `[A][C]` imzası + onaylanan bloğun numarası (16-bit, big-endian)

---

## 🛠️ Gerçeklenen İş Parçacıkları (Şartname maddeleri)

| # | İş Parçacığı | Nerede gerçeklendi |
|---|--------------|---------------------|
| 1 | **Parçalama** — firmware'i 48 byte bloklara böl | `udp-client.c → send_chunk()` |
| 2 | **Sıralama** — her bloğa blok no taşı | Başlık `[2-3]` blok no |
| 3 | **Parça doğrulama** — blok başına XOR checksum | `checksum8()` (her iki tarafta) |
| 4 | **Tüm-imaj doğrulama** — CRC32 | `crc32_stream_*()` (alıcıda) |
| 5 | **Yeniden gönderim** — timeout + retry | `TIMEOUT_SEC`, `MAX_RETRIES` |
| 6 | **Pencereleme** — stop-and-wait | `PROCESS_WAIT_EVENT_UNTIL(... \|\| !waiting_ack)` |
| 7 | **Durum yönetimi** — eksik/gelen blok takibi | `next_expected`, duplikat kontrolü |
| + | **Kalıcı depolama** — flash benzeri saklama | CFS/Coffee (`cfs_write`) |
| + | **OTA metadata** — Slot A/B güncelleme | `ota_metadata_*()` |

### Parçalama ve Paket Oluşturma (`udp-client.c → send_chunk`)

Gönderici, firmware'i 48 byte'lık bloklara böler ve her bloğun başına 8 byte'lık başlık
ekler. Aşağıdaki kod doğrudan `send_chunk` fonksiyonumuzdandır:

```c
static void
send_chunk(const uip_ipaddr_t *dest, uint16_t chunk_no)
{
  static uint8_t packet[HEADER_LEN + CHUNK_SIZE];
  uint32_t offset;
  uint8_t  payload_len;
  uint8_t  cs;

  /* Bu blogun firmware'deki baslangic yeri */
  offset = (uint32_t)chunk_no * CHUNK_SIZE;

  /* Son blok kismi olabilir (tam bolunmuyor) */
  if(offset + CHUNK_SIZE > FIRMWARE_PAYLOAD_LEN) {
    payload_len = (uint8_t)(FIRMWARE_PAYLOAD_LEN - offset);
  } else {
    payload_len = CHUNK_SIZE;
  }

  /* Firmware verisini pakete kopyala */
  memcpy(&packet[HEADER_LEN], &firmware_payload[offset], payload_len);

  /* Checksum hesapla (sadece payload uzerinden) */
  cs = checksum8(&packet[HEADER_LEN], payload_len);

  /* ── PAKET BASLIGI (8 byte) ──
   *  [0-1] Magic | [2-3] Blok no | [4-5] Toplam | [6] Boyut | [7] Checksum */
  packet[0] = (uint8_t)(OTA_MAGIC >> 8);
  packet[1] = (uint8_t)(OTA_MAGIC);
  packet[2] = (uint8_t)(chunk_no >> 8);
  packet[3] = (uint8_t)(chunk_no);
  packet[4] = (uint8_t)(TOTAL_CHUNKS >> 8);
  packet[5] = (uint8_t)(TOTAL_CHUNKS);
  packet[6] = payload_len;
  packet[7] = cs;

  simple_udp_sendto(&udp_conn, packet, HEADER_LEN + payload_len, dest);
}
```

> Çok-byte alanlar **big-endian** (üst byte önce) yazılır; alıcı aynı sırayla okur.
> Son blok 48'e tam bölünmezse `payload_len` kalan kadar küçülür.

---

## 🔑 Kullanılan Algoritmalar (Kendi Kodumuzdan)

Sistem **iki katmanlı doğrulama** kullanır: blok başına hızlı XOR checksum + tüm imaj
için güçlü CRC32 hash. Aşağıdaki kod parçaları doğrudan kaynak dosyalarımızdan alınmıştır.

### 1. XOR Checksum (blok başına — hızlı ön kontrol)

> Kaynak: `udp-server.c` (alıcı) ve `udp-client.c` (gönderici) — birebir aynı fonksiyon.

```c
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
```

- **Amaç:** Tek blokta **bit hatası** tespiti — ucuz ve hızlı.
- **Nasıl:** Bloğun tüm yük byte'larını XOR'layıp tek byte üretir. Gönderici bu değeri
  paket başlığının 8. byte'ına koyar; alıcı aynı işlemi yapıp karşılaştırır.
- **Sınırı:** İki bit aynı konumda bozulursa XOR değişmez, hatayı kaçırır. Bu yüzden
  **tek başına yetmez** — tüm imaj ayrıca CRC32 ile doğrulanır.

### 2. CRC32 (tüm imaj — güçlü bütünlük hash'i) ⭐

> Kaynak: `udp-server.c` — alıcı, bloklar geldikçe streaming (akan) CRC32 hesaplar.

```c
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
```

**Teori — bu kod ne yapıyor?**

- **CRC = Cyclic Redundancy Check (Döngüsel Artıklık Denetimi).** Veri, sabit bir
  polinoma GF(2) (modulo-2) aritmetiğinde **bölünür**; bölmenin **kalanı** 32-bit CRC
  değeridir. `crc32_stream_update` içindeki "kaydır + koşullu XOR" döngüsü tam bu
  polinom bölmesini bit bit gerçekler.
- **Polinom `0xEDB88320`:** IEEE 802.3 (Ethernet), ZIP, PNG, gzip ile **aynı** standart
  CRC32 polinomu (ters/reflected gösterim).
- **Başlangıç `0xFFFFFFFF`, bitiş `~crc` (ters çevirme):** standart CRC32 uzlaşımı —
  baştaki ve sondaki sıfırları da yakalayabilmek için.
- **Neden CRC32, neden basit checksum değil?** CRC32 **burst (ardışık) hataları**, byte
  sırası değişimlerini ve eksik blokları yakalar; basit toplama/XOR bunları kaçırabilir.
  Yanlış pozitif olasılığı 1/2³² ≈ pratikte sıfır.
- **Neden streaming?** 129 KB'lık firmware Z1'in 8 KB RAM'ine sığmaz. Bloklar **sırayla**
  geldiğinden, her blok gelince `crc32_stream_update` ara değeri günceller; tüm imaj
  hafızada biriktirilmeden son blokta `crc32_stream_final` ile sonuçlandırılır.
- ⚠️ **Önemli ayrım:** CRC32 bir **hata-tespit hash'idir**, **kriptografik değildir**.
  Kötü niyetli (kasıtlı) değişikliğe karşı SHA-256 gibi kriptografik bir hash gerekir.
  Bu projede amaç **iletim bütünlüğü** olduğundan CRC32 uygundur.

Alıcı, transfer bitince hesaplanan CRC32'yi loglar (`udp-server.c`):

```c
fw_crc = crc32_stream_final();
LOG_INFO("Hesaplanan CRC32: 0x%08lx\n", (unsigned long)fw_crc);
```

---

## 🚦 Protokol: Stop-and-Wait + Yeniden Gönderim

Gönderici bir blok yollar, **ACK gelene kadar bekler**, sonra bir sonrakine geçer.
Aşağıdaki ana döngü doğrudan `udp-client.c → PROCESS_THREAD(ota_sender_process)`'tendir:

```c
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
```

**Kilit satır** `PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&timer) || !waiting_ack)`:
süreç ya 5 sn timeout dolunca (ACK gelmedi → tekrar gönder) ya da ACK callback'i
`waiting_ack`'i sıfırlayınca (devam et) uyanır.

ACK callback'i (`udp-client.c`), doğru bloğun ACK'i gelince süreci hemen uyandırır:

```c
static void
udp_rx_callback(struct simple_udp_connection *c,
                const uip_ipaddr_t *sender_addr, uint16_t sender_port,
                const uip_ipaddr_t *receiver_addr, uint16_t receiver_port,
                const uint8_t *data, uint16_t datalen)
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
    process_poll(&ota_sender_process); /* Surecimizi uyandir: "devam et!" */
  }
}
```

| Parametre | Değer | Anlam |
|-----------|-------|-------|
| `TIMEOUT_SEC` | 5 sn | ACK için bekleme süresi |
| `MAX_RETRIES` | 5 | Bir blok için maksimum tekrar |
| `CHUNK_SIZE` | 48 byte | Blok yük boyutu |

> **Protothread notu:** Tüm durum değişkenleri (`current_chunk`, `waiting_ack`,
> `retries`, `timer`, `dest_ipaddr`) `static`'tir — çünkü Contiki protothread'leri
> yığınsızdır ve `PROCESS_WAIT` sırasında yerel değişkenleri kaybeder. Ayrıca
> `PROCESS_END()` yalnızca fonksiyonun en sonunda bir kez bulunur; erken çağrılırsa
> `switch-case` yapısı bozulup derleme hatası verir.

---

## 🛡️ Alınan Önlemler (Robustness)

| Önlem | Nasıl | Kod |
|-------|-------|-----|
| **Geçersiz paket koruması** | Magic + boyut kontrolü | `if(magic != OTA_MAGIC) return;` |
| **Bozuk blok tespiti** | XOR checksum doğrulama | `if(calc_cs != recv_cs) return;` (ACK yollanmaz → tekrar gelir) |
| **Tampon taşması koruması** | Boyut tutarlılığı | `if(datalen != HEADER_LEN + payload_len) return;` |
| **Duplikat blok** | Sıra takibi | `if(chunk_no < next_expected)` → sadece ACK, CRC bozulmaz |
| **Sıra dışı blok** | Beklenen no kontrolü | `if(chunk_no != next_expected) return;` |
| **CFS yazma hatası** | Dönüş değeri kontrolü | `if(written != payload_len) return;` |
| **Tüm-imaj bütünlüğü** | CRC32 | Son blokta `crc32_stream_final()` |
| **Takılma koruması** | MAX_RETRIES | 5 denemede gitmeyen blok atlanır |

Alıcıda bozuk blok gelince **kasıtlı olarak ACK gönderilmez** — gönderici timeout sonrası
aynı bloğu tekrar yollar (otomatik düzeltme).

Alıcının (`udp-server.c → udp_rx_callback`) gerçek doğrulama zinciri — her paket sırayla
bu kapılardan geçer:

```c
/* 1. Minimum uzunluk */
if(datalen < HEADER_LEN) return;

/* 2. Magic number (bu gerçekten OTA paketi mi?) */
magic = ((uint16_t)data[0] << 8) | data[1];
if(magic != OTA_MAGIC) return;

/* 3. Header ayristirma */
chunk_no     = ((uint16_t)data[2] << 8) | data[3];
total_chunks = ((uint16_t)data[4] << 8) | data[5];
payload_len  = data[6];
recv_cs      = data[7];

/* 4. Boyut tutarliligi (tampon tasmasi korumasi) */
if(datalen != (uint16_t)(HEADER_LEN + payload_len)) return;

/* 5. XOR checksum dogrulama (bozuksa ACK YOK -> tekrar gelir) */
calc_cs = checksum8(&data[HEADER_LEN], payload_len);
if(calc_cs != recv_cs) return;

/* 7. Duplikat blok: sadece ACK don, CRC'yi bozma */
if(chunk_no < next_expected) { /* ...ACK gonder... */ return; }

/* 8. Sira disi blok: atla */
if(chunk_no != next_expected) return;
```

Bu sıralı kapı yapısı, sadece **geçerli + doğru sıradaki + bozulmamış** bloğun CFS'e
yazılıp CRC'ye dahil edilmesini garanti eder.

---

## 💾 Kalıcı Depolama ve OTA Metadata

Alıcı (Düğüm 1) her bloğu **CFS (Coffee File System)** ile kalıcı belleğe yazar.
Aşağıdaki kod doğrudan `udp-server.c → udp_rx_callback`'tendir (hata kontrolleriyle):

```c
/* Coffee File System: Z1'deki harici flash bellek (M25P16, 2MB).
 * Blok N -> offset = N * 48 konumuna yaziyoruz. */
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
  LOG_INFO("CFS yazma HATASI! %d/%u byte (blok %u)\n", written, payload_len, chunk_no);
  return;
}
```

Transfer tamamlanınca **dual-slot OTA metadata** güncellenir. Aşağıdaki kod
`udp-server.c → PROCESS_THREAD` sonundandır:

```c
memset(&metadata, 0, sizeof(metadata));
metadata.magic       = OTA_IMAGE_MAGIC;
metadata.active_slot = OTA_SLOT_A;

/* Slot B'yi VERIFIED yap: boyut + CRC kaydet */
ota_metadata_mark_verified(&metadata, OTA_SLOT_B, NEW_FW_VERSION, total_fw_size, fw_crc);

/* Slot B'yi PENDING yap: bir sonraki boot'ta aktif olacak */
ota_metadata_stage_verified_image(&metadata, OTA_SLOT_B);
```

- **Slot A** = şu an çalışan firmware
- **Slot B** = yeni indirilen firmware → `VERIFIED` → `PENDING`
- Gerçek donanımda bir sonraki reboot'ta bootloader Slot B'yi aktive eder
  *(reboot Bölüm 1 kapsamı dışıdır; metadata güncellemesi yapılır)*

---

## ⚙️ Derleme ve Çalıştırma

### Derleme (Docker + msp430 toolchain)

```bash
cd contiki-ng/examples/rpl-udp
make -j4 TARGET=z1            # udp-client.z1 ve udp-server.z1 üretir
```

> **Not:** `Makefile`'da `CFLAGS += -Os` zorunludur. msp430-gcc 4.7.4, `-O0`'da
> `static inline` fonksiyonların yerel kopyasını üretmediğinden `mac_call_sent_callback`
> linker hatası verir; `-Os` optimizer inline'ı gerçekleştirir ve sorunu çözer.

### Cooja'da Çalıştırma

```bash
contiker cooja                          # Docker + WSLg GUI
# File > Open Simulation > BIL304-OS-Project-1.csc > Start
```

Veya başsız (headless) test:
```bash
cooja --no-gui --autostart BIL304-OS-Project-1.csc
```

---

## ✅ Beklenen Çıktı (Cooja Log)

```
[2] OTA Sender: 4800 byte, 100 blok
[3] Dugum 3: relay modunda, OTA gonderici degil.
[2] Blok 1/100 gonderildi | offset=0 | 48 byte | cs=0x82
[1] Blok 1/100 | offset=0 | 48 byte | cs=OK | toplam=1
[2] ACK alindi: Blok 0 onaylandi
   ...
[2] Blok 100/100 gonderildi | offset=4752 | 48 byte | cs=0xad
[1] Blok 100/100 | offset=4752 | 48 byte | cs=OK | toplam=100
[1] Hesaplanan CRC32: 0xd1c528aa
[1] Yuklenmeye hazir yeni firmware alimi tamamlandi!
[1] Slot B VERIFIED: v2 boyut=4800 crc=0xd1c528aa
[1] Slot B PENDING: sonraki acilista yeni firmware aktif olacak.
[2] === OTA TRANSFERI TAMAMLANDI! ===
```

---

## 📂 Dosya Yapısı

| Dosya | İçerik |
|-------|--------|
| `udp-client.c` | OTA gönderici (Düğüm 2) + relay (Düğüm 3) |
| `udp-server.c` | OTA alıcı (Düğüm 1): CFS + CRC32 + metadata |
| `ota-metadata.h/.c` | Dual-slot boot metadata kütüphanesi (hoca tarafından sağlandı) |
| `firmware_data.h` | Test firmware örneği (hex dizi olarak gömülü payload) |
| `BIL304-OS-Project-1.csc` | Cooja simülasyon senaryosu (3 Z1 düğüm) |
| `Makefile` | Derleme yapılandırması (`-Os` düzeltmesi dahil) |

---

*BIL 304 — İşletim Sistemleri — Bahar 2025/2026*
