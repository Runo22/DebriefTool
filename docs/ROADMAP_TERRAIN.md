# Yol Haritası — Gerçek Arazi (DEM) Verisi: Türkiye + Ankara / Pist Bölgeleri

Hedef: Bugünkü **prosedürel sinüs arazisinin** (`Application::draw_terrain()` →
`base_height`) yerine, repo ile birlikte gelen **sıkıştırılmış ikili bir yükseklik
paketinden** (DEM) gerçek Türkiye arazisini üretmek.

- **Ankara çevresi** ve **Akıncı (LTAE)** başta olmak üzere pistlerin etrafı
  **yüksek çözünürlüklü**,
- Türkiye'nin geri kalanı **dosya boyutunu şişirmeyen** kaba çözünürlükte,
- Toplam paket **~35–45 MB** (git'e veya Release/LFS'e sığar).

> **Durum (güncel):** Faz 0–4 tamamlandı.
> - Veri + bağımsız okuma kütüphanesi: [`terrain/`](../terrain/) (`tdem`; tek dosya
>   `terrain.tdem`, 49 MB; C++ okuyucu, Python üretici, testler). Komşu ülkeler
>   (Yunanistan, Bulgaristan, Kıbrıs, Gürcistan, Ermenistan, Azerbaycan, Suriye,
>   Irak, …) 30″ (~900 m) bölgesel katmanla eklendi (+4.4 MB); Türkiye'nin
>   ~100–150 km yakın çevresi (Kıbrıs, kuzey Suriye/Irak, Kafkasya, Ege adaları)
>   Türkiye ile aynı 7.5″ katmanda.
> - Gerçekleşen katmanlar: Türkiye 7.5″, Ankara 1″, İncirlik/Çiğli 1″ (40 km),
>   **Akıncı 5 m (16 km), İncirlik 5 m, Çiğli 5 m (12 km)**; pistler OurAirports
>   geometrisiyle düzleştirildi (eşik kotları yayınlanan değerlerle 1–3 m içinde).
> - AfterAction: quadtree LOD arazi (`src/terrain/TerrainSystem`), pist çizimleri,
>   AGL, imleç altı kot, hillshade mini harita, kayıtlarda orijin.
> - Dünya eğriliği bilinçli olarak uygulanmadı: arazi ve izler aynı düz
>   eşdikdörtgen projeksiyonu paylaştığı için birbirlerine göre doğrular;
>   orijin >250 km uzağa veri gelince yeniden ortalanır.
> - Açık kalanlar: Faz 5'te Release asset'e taşıma (şimdilik paketler doğrudan
>   git'te), OSM taksi yolu/apron verisi, varsa kurumsal yüksek çözünürlüklü veri.
>
> Aşağısı orijinal plandır.

---

## 1. Bugün neredeyiz

| Konu | Mevcut durum | Dosya |
|---|---|---|
| Arazi yüksekliği | 4 sinüs teriminin toplamı (`base_height`), gerçek veri yok | `src/app/Application.cpp` (`draw_terrain`, `terrain_height_at`) |
| Mesh | Her karede kamerayı merkez alan 80×80 grid, 4 LOD bandı + geomorph | aynı |
| Koordinat | Yerel düz-dünya: `x = Δlon·cos(lat0)·111319.9`, `z = −Δlat·111319.9`, `y = alt (MSL)` | `lat_lon_to_enu()` |
| Orijin | İlk gelen paketin lat/lon'u; demo'da (0,0) | `ensure_origin_and_convert()` |
| Kayıt | `SceneOrigin` alanı var ama **hep boş (0,0)** yazılıyor | `src/persistence/Recorder.cpp` |
| İrtifa referansı | Protokol: **metre MSL** | `docs/PROTOCOL.md` |

İki iyi haber:
1. Protokol irtifası **MSL** → Copernicus DEM yükseklikleri de **EGM2008 (MSL)**.
   Geoid düzeltmesi gerekmez; uçak irtifası ile arazi aynı referansta.
   (`BinaryFormat.hpp`'deki "above ellipsoid" yorumu yanlış — düzeltilmeli.)
2. raylib zaten **DEFLATE** (`CompressData` / `DecompressData`) içeriyor →
   sıkıştırma için **yeni bağımlılık gerekmez**.

---

## 2. Veri kaynakları ve lisans

| Kaynak | Çözünürlük | Lisans | Not |
|---|---|---|---|
| **Copernicus GLO-30** | 1″ (~30 m) | Ücretsiz, atıfla yeniden dağıtılabilir | **Ana kaynak.** AWS açık veri: `copernicus-dem-30m` (1°×1° COG GeoTIFF). DSM'dir (bina/ağaç dahil). |
| Copernicus GLO-90 | 3″ (~90 m) | aynı | Türkiye geneli kaba katman için doğrudan kullanılabilir. |
| SRTM 1″ | ~30 m | Kamu malı | Yedek kaynak; pist çevresinde radar gürültüsü fazla. |
| FABDEM | ~30 m, bina/ağaç ayıklanmış | **CC BY-NC-SA (ticari değil)** | Daha temiz zemin; lisansı projeye uygunsa. |
| HGM / LiDAR / DTED-2 | 2–10 m | Kısıtlı / kurumsal | Varsa boru hattı **GeoTIFF olarak kabul eder** (bkz. Faz 1). |
| OpenStreetMap `aeroway=runway/taxiway/apron` | vektör | ODbL | Pist düzleştirme (runway burn-in) için. |

**Dürüst not:** Açık veride Türkiye için 30 m'den iyisi yok. Pist çevresindeki
"yüksek çözünürlük" katmanı (10 m / 5 m), 30 m verinin **bikübik yeniden
örneklenmesi + OSM pist poligonlarının düz ve doğru kotla gömülmesiyle** üretilir.
Bu ekranda pürüzsüz yüzey ve düz, doğru yükseklikte pist sağlar ama gerçekte
olmayan ayrıntı eklemez. Kurumsal yüksek çözünürlüklü veri gelirse aynı katmana
doğrudan yazılır.

---

## 3. Çözünürlük katmanları ve boyut bütçesi

Coğrafi (lat/lon) grid, int16 metre (veya 0.25 m quantize), karo başına öngörücü
delta + DEFLATE. DEM'lerde tipik sıkıştırma **3–5×**.

| Katman | Kapsam | Çözünürlük | Ham | Sıkıştırılmış (tahmin) |
|---|---|---|---|---|
| **L0 — Türkiye** | 35.5–42.5 N, 25.5–45.0 E (+ komşu kıyılar), deniz karoları atlanır | 7.5″ (~230 m) | ~29 MB | **~7–9 MB** |
| **L1 — Ankara bölgesi** | 39.4–40.6 N, 32.0–33.6 E (~133×136 km) | 1″ (~30 m) | ~50 MB | **~12–18 MB** |
| **L1 — Diğer üsler** | Her pist çevresinde ~30×30 km | 1″ (~30 m) | ~2.4 MB/üs | **~0.6 MB/üs** (15 üs ≈ 9 MB) |
| **L2 — Pist yamaları** | Akıncı, Esenboğa, Etimesgut, Güvercinlik: ~11×11 km | 1/3″ (~10 m) | ~3 MB/yama | **~0.8 MB/yama** |
| (Ops.) L2+ Akıncı | ~6×6 km | 0.2″ (~5 m) | ~2.5 MB | ~0.7 MB |
| Önizleme piramidi | L0/L1'in 2×,4×… küçültmeleri | — | +%33 | ~+4 MB |

**Toplam ≈ 35–45 MB.** Karşılaştırma: tüm Türkiye 30 m ≈ 1.7 GB ham; 90 m ≈ 180 MB
ham — ikisi de repo için gereksiz büyük. Kademeli yapı aynı görsel kaliteyi
uçuşun yoğun olduğu yerde verip toplamı ~40× küçültür.

### Bölge listesi (konfigürasyonla yönetilir)

Koordinatlar yaklaşık; boru hattı OSM'deki pist poligonlarından kesinleştirir.

| ICAO | Üs / Havalimanı | Lat | Lon | Katman |
|---|---|---|---|---|
| LTAE | **Akıncı (Mürted)** | 40.079 | 32.566 | L1 + L2 (+5 m) |
| LTAC | Esenboğa | 40.128 | 32.995 | L1 + L2 |
| LTAD | Etimesgut | 39.950 | 32.689 | L1 + L2 |
| LTAB | Güvercinlik | 39.935 | 32.741 | L1 + L2 |
| LTBI | Eskişehir | 39.784 | 30.583 | L1 |
| LTAN | Konya | 37.979 | 32.562 | L1 |
| LTAP | Merzifon | 40.829 | 35.522 | L1 |
| LTAG | İncirlik | 37.002 | 35.426 | L1 |
| LTAU | Kayseri Erkilet | 38.770 | 35.495 | L1 |
| LTAT | Malatya Erhaç | 38.435 | 38.091 | L1 |
| LTCC | Diyarbakır | 37.894 | 40.201 | L1 |
| LTCJ | Batman | 37.929 | 41.116 | L1 |
| LTCE | Erzurum | 39.957 | 41.170 | L1 |
| LTBF | Balıkesir | 39.620 | 27.926 | L1 |
| LTBG | Bandırma | 40.318 | 27.978 | L1 |
| LTBU | Çorlu | 41.138 | 27.919 | L1 |
| LTBS | Dalaman | 36.713 | 28.793 | L1 |

Bu tablo `assets/terrain/regions.yaml` dosyasına taşınır; yeni bölge eklemek =
satır ekleyip paketi yeniden üretmek.

---

## 4. Dosya formatı — `.aterrain` (ikili, mmap'lenebilir)

Neden GeoTIFF değil: GDAL/libtiff bağımlılığı, çalışma anında karo bazlı hızlı
erişim yok. Kendi basit formatımız, mevcut `BinaryFormat.hpp` üslubunda:

```
[ Header 64 B ]
  magic "AATR", version, flags
  vertical_datum  = EGM2008 (MSL)
  tile_samples    = 256   (+1 kenar örneği → 257×257, komşu karolar kenarı paylaşır)
  level_count, tile_count, index_offset
[ LevelDesc × level_count ]
  arcsec_per_sample (float64), bbox (lat0,lon0,lat1,lon1)
[ TileIndex × tile_count ]   (level, ty, tx) ile sıralı → ikili arama
  level u8, tx u16, ty u16, codec u8
  offset u64, size u32
  h_min i16, h_max i16          ← LOD seçimi / culling için, açmadan bilinir
[ Tile blob'ları ]
  quant_step (örn. 0.25 m), base = h_min
  değerler = (h − base)/quant_step → uint16
  2B öngörücü (sol + üst − sol-üst) → zigzag → DEFLATE
```

- **Tek bir "deniz" karosu** yoktur: index'te olmayan karo = 0 m (deniz) veya bir
  alt seviyeden örneklenir.
- Dosyalar bölge bazlı bölünebilir (`tr_base.aterrain`, `ankara.aterrain`,
  `airbases.aterrain`) → her biri < 50 MB (GitHub uyarı sınırı).
- Çalışma anında: dosya **mmap** (veya tek seferde okunur), açılmış karolar
  **LRU önbellek**te (örn. 256 karo × 132 KB ≈ 34 MB RAM).

### Örnekleme API'si

```cpp
namespace terrain {
class DemPack {
public:
    bool  open(const std::filesystem::path& dir);      // *.aterrain hepsi
    // En ince mevcut seviyeden bilineer örnek. Kapsam dışı → nullopt.
    std::optional<float> height_msl(double lat, double lon);
    // LOD istenen çözünürlüğe en yakın seviyeden (mesh üretimi için).
    std::optional<float> height_msl(double lat, double lon, double max_arcsec);
};
}
```

---

## 5. Fazlar

### Faz 0 — Hazırlık (küçük, hemen yapılabilir)
- [ ] `SceneOrigin`'i kayıtlara **gerçekten yaz** (şu an hep 0,0) → oynatmada aynı
      arazi yüklenebilsin. Eski kayıtlarda 0,0 = "bilinmiyor".
- [ ] `BinaryFormat.hpp` yorumunu düzelt: irtifa **MSL**, elipsoit değil.
- [ ] `lat_lon_to_enu`'nun **tersini** ekle (`enu_to_lat_lon`) — arazi mesh'i
      dünya (x,z) → (lat,lon) ile örneklenecek.
- [ ] Orijin seçeneği: "ilk paket" (bugünkü) / "sabit" (örn. Akıncı) — Settings + config.

### Faz 1 — Paket üretim aracı (`tools/terrain_build.py`, çevrimdışı)
- [ ] Copernicus GLO-30 / GLO-90 1°×1° karolarını indir (AWS açık veri, HTTPS).
- [ ] `regions.yaml`'ı oku; her katman için hedef grid'e yeniden örnekle
      (`rasterio`/GDAL, bikübik; L0 için ortalama alma).
- [ ] **Pist gömme:** OSM'den `aeroway=runway|taxiway|apron` poligonlarını çek;
      pist eksenindeki DEM medyanından düz (veya iki uç arasında doğrusal eğimli)
      yüzey oluştur, kenarlarda 50–100 m yumuşak geçişle L1/L2'ye yaz.
- [ ] Kullanıcı GeoTIFF'i kabul et (`--override region=LTAE file=x.tif`) — kurumsal
      yüksek çözünürlüklü veri gelirse.
- [ ] Format yazıcı + **doğrulama**: rastgele noktalarda kaynak ile paket farkı
      ≤ quant_step; boyut raporu.
- [ ] Çıktı: `assets/terrain/*.aterrain` + `ATTRIBUTION.txt` (Copernicus/OSM atıfı).

### Faz 2 — Okuyucu (C++)
- [ ] `src/terrain/DemPack.{hpp,cpp}`: header/index okuma, karo açma
      (`DecompressData`), predictor tersi, LRU önbellek.
- [ ] Birim test: araçla üretilmiş küçük test paketi → bilinen noktaların değeri.
- [ ] Karo açmayı **iş parçacığına** taşı (model yüklemedeki desen: CPU işi
      arka planda, GPU yüklemesi ana thread'de — bkz. `ROADMAP_MODELS.md`).

### Faz 3 — Arazi render'ının DEM'e bağlanması
- [ ] `base_height(wx,wz)` → `enu_to_lat_lon` → `DemPack::height_msl`. Kapsam
      dışında / demo modunda prosedürel araziye (veya düz 0 m) düş.
- [ ] `terrain_height_at()` aynı kaynağı kullansın (yer halkaları, drop-line'lar,
      kamera-zemin çarpışması zaten buna bağlı).
- [ ] **Anlık 80×80 grid → karo tabanlı mesh (CDLOD):** her karo için bir kez
      `Mesh` üret ve GPU'ya yükle, kameraya uzaklığa göre quadtree'den seç;
      mevcut geomorph mantığı korunur; karo kenarlarına "etek" (skirt) ile çatlak
      gizleme. Hedef: Ankara 30 m katmanında 60 FPS.
- [ ] **Dünya eğriliği:** ENU düz-dünya, 100 km'de ~785 m, 300 km'de ~7 km hata
      verir. Mesh köşelerine `y −= d²/(2R)` uygula (uçaklar da aynı düzeltmeyi
      alır ya da yalnızca arazi uzakta "batar" — ufuk doğal görünür). Türkiye
      ölçeğinde (1500 km) bu zorunlu.
- [ ] Renk: hipsometrik palet + normal'den hillshade; L2 bölgede pist poligonu
      koyu gri.

### Faz 4 — Arayüz ve araçlar
- [ ] Settings → Terrain: kaynak (Prosedürel / DEM), paket klasörü, yükseklik
      abartısı (mevcut `terrain_height_scale`), aktif katman göstergesi.
- [ ] Track panelinde **AGL** (MSL − arazi) göster; AGL < 0 uyarısı (veri/datum
      hatalarını yakalamak için faydalı).
- [ ] Demo modu için "Akıncı senaryosu": orijin LTAE, uçaklar gerçek pistten kalkar.
- [ ] Hangar benzeri `terrain_view` aracı (ops.): paketi tek başına incele.

### Faz 5 — Dağıtım
Paketin repoda nasıl tutulacağı (biri seçilmeli):

| Seçenek | Artı | Eksi |
|---|---|---|
| **Düz git** (dosya başı < 50 MB) | En basit, `clone` yeter | Her yeniden üretim geçmişi büyütür |
| **Git LFS** | Geçmiş şişmez | GitHub ücretsiz kota: 1 GB depolama / 1 GB bant/ay |
| **GitHub Release asset** + `scripts/bootstrap` indirir | Repo küçük kalır, 2 GB'a kadar dosya | Ek indirme adımı |

**Öneri:** Üretim aracı ve `regions.yaml` repoda; `.aterrain` dosyaları
**Release asset** olarak, bootstrap betiği indirip `assets/terrain/`'e koyar.
Paket nadiren değişeceği için ilk sürümde düz git (~40 MB) da kabul edilebilir.

---

## 6. Riskler

| Risk | Etki | Önlem |
|---|---|---|
| Copernicus **DSM** (bina/ağaç dahil) | Ankara şehir içinde zemin ~10–30 m yüksek görünür | Pist gömme; lisans uygunsa FABDEM; uyarı notu |
| 30 m → 10/5 m yeniden örnekleme | Gerçek ayrıntı eklemez | Belgede açıkça belirtildi; kurumsal veri yolu açık |
| Düz-dünya ENU | Uzakta yükseklik/konum hatası | Faz 3 eğrilik düzeltmesi; orijini bölge merkezine sabitleme |
| float hassasiyeti (orijinden > ~100 km) | Titreme | Kamera-göreli render veya bölge değişince orijin kaydırma |
| Paket büyümesi | Repo şişer | Katmanlı bütçe (§3), Release asset |
| Lisans | Dağıtım hakkı | Copernicus/OSM atıfları `ATTRIBUTION.txt`'de; FABDEM yalnız ticari olmayan kullanım |

---

## 7. Önerilen sıra

1. Faz 0 (yarım gün) — kayıt orijini + ters dönüşüm.
2. Faz 1 yalnız **Akıncı L1+L2** ile uçtan uca (~3 MB paket) → format sabitlenir.
3. Faz 2 + Faz 3'ün "örnekleme" kısmı (mevcut 80×80 grid ile) → ilk görüntü.
4. Ankara L1 + Türkiye L0 eklenir; boyut raporu bütçeyle karşılaştırılır.
5. CDLOD karo mesh'i + eğrilik düzeltmesi.
6. Diğer üsler, UI, dağıtım.
