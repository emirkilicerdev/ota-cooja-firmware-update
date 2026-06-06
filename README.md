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

---

## 🔑 Kullanılan Algoritmalar (Teori)

Sistem **iki katmanlı doğrulama** kullanır: blok başına hızlı XOR checksum + tüm imaj
için güçlü CRC32 hash.

### 1. XOR Checksum (blok başına — hızlı ön kontrol)

Her bloğun yük byte'larının XOR'u alınarak 1 byte üretilir:

```c
static uint8_t checksum8(const uint8_t *data, uint8_t len) {
  uint8_t cs = 0;
  for(uint8_t i = 0; i < len; i++) {
    cs ^= data[i];          /* tüm byte'ları XOR'la */
  }
  return cs;
}
```

- **Amaç:** Tek blokta **bit hatası** tespiti — ucuz ve hızlı.
- **Sınırı:** İki bit aynı konumda bozulursa XOR değişmez, hatayı kaçırır. Bu yüzden
  **tek başına yetmez** — tüm imaj CRC32 ile ayrıca doğrulanır.

### 2. CRC32 (tüm imaj — güçlü bütünlük hash'i) ⭐

**CRC = Cyclic Redundancy Check (Döngüsel Artıklık Denetimi).** Veri, sabit bir polinoma
GF(2) (modulo-2) aritmetiğinde **bölünür**; **kalan** 32-bit CRC değerini verir.

- **Polinom:** `0xEDB88320` — IEEE 802.3 (Ethernet), ZIP, PNG, gzip ile **aynı** standart
  polinom (ters/reflected gösterim).
- **Algoritma:**

```c
static void crc32_stream_init(void)  { crc_state = 0xFFFFFFFFu; }   /* başlangıç */

static void crc32_stream_update(const uint8_t *data, uint8_t len) {
  while(len--) {
    crc_state ^= *data++;                       /* byte'ı al */
    for(uint8_t i = 0; i < 8; i++) {            /* 8 bit işle */
      if(crc_state & 1u)
        crc_state = (crc_state >> 1) ^ 0xEDB88320u;  /* polinomla XOR */
      else
        crc_state >>= 1;
    }
  }
}

static uint32_t crc32_stream_final(void) { return ~crc_state; }     /* bitiş: ters çevir */
```

- **Neden CRC32, neden basit checksum değil?** CRC32, **burst (ardışık) hataları**, byte
  sırası değişimlerini ve eksik blokları yakalar; basit toplama/XOR bunları kaçırabilir.
  Yanlış pozitif olasılığı 1/2³² ≈ pratikte sıfırdır.
- **Streaming (akan) hesaplama:** 129 KB'lık firmware Z1'in 8 KB RAM'ine sığmaz. Bloklar
  **sırayla** geldiğinden, her blok gelince CRC ara değeri güncellenir; tüm imaj hafızada
  biriktirilmeden son blokta sonuçlandırılır.
- ⚠️ **Önemli ayrım:** CRC32 bir **hata-tespit hash'idir**, **kriptografik değildir**.
  Kötü niyetli (kasıtlı) değişikliğe karşı koruma için SHA-256 gibi kriptografik bir hash
  gerekir. Bu projede amaç **iletim bütünlüğü** olduğundan CRC32 uygundur.

---

## 🚦 Protokol: Stop-and-Wait + Yeniden Gönderim

Gönderici bir blok yollar, **ACK gelene kadar bekler**, sonra bir sonrakine geçer:

```c
while(!transfer_done) {
  if(current_chunk >= TOTAL_CHUNKS) { transfer_done = 1; break; }   /* hepsi gitti */

  if(waiting_ack) {                       /* önceki bloğa ACK gelmedi mi? */
    retries++;
    if(retries > MAX_RETRIES) {           /* 5 denemede gitmezse atla */
      current_chunk++; waiting_ack = 0; retries = 0; continue;
    }
  }

  send_chunk(&dest_ipaddr, current_chunk);    /* bloğu gönder */
  waiting_ack = 1;

  etimer_set(&timer, TIMEOUT_SEC * CLOCK_SECOND);   /* 5 sn timeout kur */
  PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&timer) || !waiting_ack);
  /* ya timeout dolar (tekrar gönder) ya da ACK gelir (devam et) */
}
```

ACK callback'i, doğru bloğun ACK'i gelince süreci uyandırır:

```c
static void udp_rx_callback(..., const uint8_t *data, uint16_t datalen) {
  if(datalen < 4 || data[0] != 'A' || data[1] != 'C') return;   /* OTA ACK değil */
  uint16_t acked = ((uint16_t)data[2] << 8) | data[3];
  if(acked == current_chunk) {
    waiting_ack = 0; retries = 0; current_chunk++;
    process_poll(&ota_sender_process);    /* süreci hemen uyandır */
  }
}
```

| Parametre | Değer | Anlam |
|-----------|-------|-------|
| `TIMEOUT_SEC` | 5 sn | ACK için bekleme süresi |
| `MAX_RETRIES` | 5 | Bir blok için maksimum tekrar |
| `CHUNK_SIZE` | 48 byte | Blok yük boyutu |

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

---

## 💾 Kalıcı Depolama ve OTA Metadata

Alıcı (Düğüm 1) her bloğu **CFS (Coffee File System)** ile kalıcı belleğe yazar:

```c
fd = cfs_open(FIRMWARE_FILE, CFS_WRITE);
cfs_seek(fd, chunk_no * CHUNK_SIZE, CFS_SEEK_SET);   /* doğru offset'e */
cfs_write(fd, &data[HEADER_LEN], payload_len);
cfs_close(fd);
```

Transfer tamamlanınca **dual-slot OTA metadata** güncellenir (`ota-metadata.h`):

```c
ota_metadata_mark_verified(&metadata, OTA_SLOT_B, version, size, crc);  /* Slot B = VERIFIED */
ota_metadata_stage_verified_image(&metadata, OTA_SLOT_B);               /* Slot B = PENDING */
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
