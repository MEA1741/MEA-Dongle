/*
 * MEA-Dongle v4.2 - Bug Hunter & Hotfix Edition
 * M5Stack CoreS3 - WireGuard VPN Dongle with USB Internet Forwarding
 * [YAMALAR]:
 * 1. Ghost Touch Isolated: Klavye geri dönüşünde yaşanan döngüsel kilitlenme engellendi.
 * 2. WiFi Stack Hard Reset: Zaman aşımından sonra WiFi yığını sıfırlanarak donma çözüldü.
 * 3. UDP Loopback: Derleme hatası veren kütüphane bağımlılığı localhost enjeksiyonu ile sabitlendi.
 */

#include <M5CoreS3.h>
#include <WiFi.h>
#include <WireGuard-ESP32.h>
#include <esp_idf_version.h>
#include <esp_netif.h>
#include <time.h>

// ─── YAPILANDIRMA ────────────────────────────────────────────────────────────

struct WifiNetwork {
    const char* ssid;
    const char* password;
};

WifiNetwork bilinenAglar[] = {
    {"Ev_Wifi",         "evsifresi123"},
    {"Telefon_Hotspot", "telefonsifresi"}
};
const int AG_SAYISI = sizeof(bilinenAglar) / sizeof(bilinenAglar[0]);

const char* WG_PRIVATE_KEY      = "KENDI_PRIVATE_KEYIN";
const IPAddress WG_LOCAL_IP(10, 0, 0, 2);
const IPAddress WG_SUBNET(255, 255, 255, 0);
const IPAddress WG_GATEWAY(10, 0, 0, 1);
const char* WG_ENDPOINT_ADDRESS = "vpn.sunucu.com";
const int   WG_ENDPOINT_PORT    = 51820;
const char* WG_PUBLIC_KEY       = "SUNUCUNUN_PUBLIC_KEYIN";

const unsigned long WIFI_TIMEOUT_MS    = 15000;
const unsigned long NTP_TIMEOUT_MS     = 10000;

#define C_BASLIK  0x07FF  
#define C_OK      0x07E0  
#define C_HATA    0xF800  
#define C_UYARI   0xFFE0  
#define C_METIN   0xFFFF  
#define C_KUTU    0x2104  
#define C_SECILI  0x3166  

#define SLIP_END             0xC0
#define SLIP_ESC             0xDB
#define SLIP_ESC_END         0xDC
#define SLIP_ESC_ESC         0xDD
#define SLIP_BUFFER_SIZE     2000

// ─── GLOBAL DURUMLAR VE BELLEK YÖNETİMİ ──────────────────────────────────────

WireGuard wg;
M5Canvas canvas(&M5.Display);
WiFiUDP slipUdp; 

enum Durum {
    D_AG_TARAMA, D_AG_SECIM, D_KLAVYE_GIRIS, D_WIFI_BAGLANIYOR,
    D_NTP, D_WG_BASLAT, D_AKTIF, D_HATA
};
Durum durum = D_AG_TARAMA;

unsigned long wifiBasladi = 0;
unsigned long sonDurumDegisimZamani = 0; // Dokunmatik çakışmasını önlemek için zaman damgası
bool          wgAktif     = false;
int           agSayisi    = 0;
String        secilenSSID = "";
String        girilenSifre = "";

int scrollY = 0;
int maxScrollY = 0;
int tBaslangicY = 0;
int tSonY = 0;
bool kaydiriliyor = false;

uint8_t slipRxBuffer[SLIP_BUFFER_SIZE];
int slipRxIndex = 0;

enum KlavyeModu { K_KUCUK, K_BUYUK, K_SEMBOL };
KlavyeModu klavyeModu = K_KUCUK;

const String klavyeHaritalari[3][4][10] = {
    {
        {"1","2","3","4","5","6","7","8","9","0"},
        {"q","w","e","r","t","y","u","ı","o","p"},
        {"a","s","d","f","g","h","j","k","l","ş"},
        {"z","x","c","v","b","n","m","ö","ç","ğ"}
    },
    {
        {"!","@","#","$","%","^","&","*","(",")"},
        {"Q","W","E","R","T","Y","U","İ","O","P"},
        {"A","S","D","F","G","H","J","K","L","Ş"},
        {"Z","X","C","V","B","N","M","Ö","Ç","Ğ"}
    },
    {
        {"1","2","3","4","5","6","7","8","9","0"},
        {"-","/",":",";","(",")","$","&","@", "\""},
        {".",",","?","!", "'","[","]","{","}","#"},
        {"+","=","_","\\","|","~","<",">","*","%"}
    }
};

// ─── YARDIMCI FONKSİYONLAR ───────────────────────────────────────────────────

void staticEkranCiz(const char* baslik, const char* durumMesaj, uint16_t durumRenk = C_METIN) {
    M5.Display.clear();
    M5.Display.setTextColor(C_BASLIK);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(10, 6);
    M5.Display.print(baslik);
    M5.Display.drawLine(0, 26, M5.Display.width(), 26, C_BASLIK);
    
    M5.Display.fillRect(0, M5.Display.height() - 22, M5.Display.width(), 22, 0x10A2);
    M5.Display.setTextColor(durumRenk);
    M5.Display.setTextSize(1);
    M5.Display.setCursor(10, M5.Display.height() - 16);
    M5.Display.print(durumMesaj);
}

void wifiYigininiSifirla() {
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    delay(300);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(WIFI_PS_NONE);
    delay(200);
}

void agListesiRender() {
    canvas.clear();
    canvas.fillSprite(BLACK);
    
    if (scrollY > 15) {
        canvas.setTextColor(C_UYARI);
        canvas.setTextSize(1);
        canvas.setCursor(M5.Display.width()/2 - 70, 5);
        if (scrollY > 45) canvas.print("YENILEMEK ICIN BIRAKIN!");
        else canvas.print("Yenilemek icin asagi cekin...");
    }

    for (int i = 0; i < agSayisi; i++) {
        int y = (i * 56) + scrollY + (scrollY > 0 ? 25 : 0);
        if (y > -55 && y < 210) {
            canvas.fillRoundRect(6, y, canvas.width() - 12, 50, 6, C_KUTU);
            canvas.drawRoundRect(6, y, canvas.width() - 12, 50, 6, 0x52AA);
            canvas.setTextColor(C_METIN);
            canvas.setTextSize(2);
            canvas.setCursor(16, y + 8);
            canvas.print(WiFi.SSID(i));
            canvas.setTextSize(1);
            canvas.setTextColor(C_UYARI);
            canvas.setCursor(16, y + 34);
            canvas.printf("RSSI: %d dBm | Ch: %d", WiFi.RSSI(i), WiFi.channel(i));
        }
    }
    canvas.pushSprite(0, 28); 
}

void wifiTara() {
    durum = D_AG_TARAMA;
    sonDurumDegisimZamani = millis();
    scrollY = 0;
    staticEkranCiz("MEA-Dongle v4.2", "Havadaki WiFi sinyalleri taraniyor...", C_UYARI);
    
    wifiYigininiSifirla(); // Her tarama öncesi temiz başlangıç
    agSayisi = WiFi.scanNetworks();

    if (agSayisi <= 0) {
        staticEkranCiz("HATA", "Cevrede WiFi agi bulunamadi! Yeniden deneniyor...", C_HATA);
        delay(2000);
        wifiTara();
        return;
    }

    int toplamListeBoyutu = (agSayisi * 56);
    int ekranGorusAlani = M5.Display.height() - 50;
    maxScrollY = (toplamListeBoyutu > ekranGorusAlani) ? -(toplamListeBoyutu - ekranGorusAlani + 15) : 0;

    durum = D_AG_SECIM;
    sonDurumDegisimZamani = millis();
    staticEkranCiz("--- Bir Ag Secin ---", "Asagi cekip yenileyin veya bir aga dokunun");
    agListesiRender();
}

int dokunulanIndexBul(int touchY) {
    int tiklananY = touchY - 28 - (scrollY > 0 ? 25 : 0); 
    for (int i = 0; i < agSayisi; i++) {
        int y0 = (i * 56) + scrollY;
        if (tiklananY >= y0 && tiklananY <= y0 + 50) return i;
    }
    return -1;
}

int bilinenAgBul(const String& ssid) {
    for (int i = 0; i < AG_SAYISI; i++) {
        if (ssid == bilinenAglar[i].ssid) return i;
    }
    return -1;
}

// ─── KLAVYE SİSTEMİ ──────────────────────────────────────────────────────────

void klavyeArayuzCiz() {
    M5.Display.startWrite();
    M5.Display.fillRect(0, 28, M5.Display.width(), M5.Display.height() - 50, BLACK);
    
    M5.Display.fillRoundRect(8, 34, 45, 32, 4, C_HATA);
    M5.Display.setTextColor(C_METIN);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(20, 42);
    M5.Display.print("<");

    M5.Display.fillRoundRect(60, 34, M5.Display.width() - 68, 32, 4, C_KUTU);
    M5.Display.drawRoundRect(60, 34, M5.Display.width() - 68, 32, 4, C_BASLIK);
    M5.Display.setCursor(68, 42);
    M5.Display.setTextColor(C_METIN);
    M5.Display.print(girilenSifre + "|");

    int tGenislik = M5.Display.width() / 10 - 4;
    int tYukseklik = 28;
    
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 10; c++) {
            int x = 2 + (c * (tGenislik + 4));
            int y = 76 + (r * (tYukseklik + 4));
            M5.Display.fillRoundRect(x, y, tGenislik, tYukseklik, 3, C_SECILI);
            M5.Display.setTextColor(C_METIN);
            M5.Display.setTextSize(2);
            M5.Display.setCursor(x + (tGenislik/2) - 5, y + (tYukseklik/2) - 7);
            M5.Display.print(klavyeHaritalari[klavyeModu][r][c]);
        }
    }

    M5.Display.fillRoundRect(4, 204, 75, 34, 4, 0x7BEF);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(BLACK);
    M5.Display.setCursor(14, 215);
    if(klavyeModu == K_KUCUK) M5.Display.print("ABC (Buyuk)");
    else if(klavyeModu == K_BUYUK) M5.Display.print("sym (?@#)");
    else M5.Display.print("abc (Kucuk)");

    M5.Display.fillRoundRect(84, 204, 60, 34, 4, C_UYARI);
    M5.Display.setCursor(98, 215);
    M5.Display.print("SIL (<)");

    M5.Display.fillRoundRect(150, 204, M5.Display.width() - 154, 34, 4, C_OK);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(M5.Display.width()/2 + 20, 213);
    M5.Display.print("BAGLAN");
    
    M5.Display.endWrite();
}

void klavyeDokunmaSorgu(int tx, int ty) {
    // Geri Dönüş Butonu (<)
    if (tx >= 8 && tx <= 53 && ty >= 34 && ty <= 66) {
        scrollY = 0; 
        durum = D_AG_SECIM;
        sonDurumDegisimZamani = millis(); // Debounce kilidi tetikle
        staticEkranCiz("--- Bir Ag Secin ---", "Asagi cekip yenileyin veya bir aga dokunun");
        agListesiRender();
        delay(250); 
        return;
    }

    if (ty >= 204 && ty <= 238) {
        if (tx >= 4 && tx <= 79) { 
            klavyeModu = (klavyeModu == K_KUCUK) ? K_BUYUK : ((klavyeModu == K_BUYUK) ? K_SEMBOL : K_KUCUK);
            klavyeArayuzCiz();
        }
        else if (tx >= 84 && tx <= 144) { 
            if (girilenSifre.length() > 0) {
                girilenSifre.remove(girilenSifre.length() - 1);
                klavyeArayuzCiz();
            }
        }
        else if (tx >= 150 && tx <= M5.Display.width() - 4) { 
            if (girilenSifre.length() >= 8) {
                durum = D_WIFI_BAGLANIYOR;
                sonDurumDegisimZamani = millis();
            } else {
                staticEkranCiz("Sifre Hatasi", "Sifre en az 8 karakter olmali!", C_HATA);
                delay(1500);
                staticEkranCiz("Sifre Giriniz", "Sifrenizi girip BAGLAN butonuna basin");
                klavyeArayuzCiz();
            }
        }
        delay(150);
        return;
    }

    int tGenislik = M5.Display.width() / 10 - 4;
    int tYukseklik = 28;

    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 10; c++) {
            int x = 2 + (c * (tGenislik + 4));
            int y = 76 + (r * (tYukseklik + 4));
            if (tx >= x && tx <= x + tGenislik && ty >= y && ty <= y + tYukseklik) {
                if (girilenSifre.length() < 64) {
                    girilenSifre += klavyeHaritalari[klavyeModu][r][c];
                    klavyeArayuzCiz();
                }
                delay(150);
                return;
            }
        }
    }
}

// ─── SLIP NETWORKING ENGINE ──────────────────────────────────────────────────

void slipPaketleriniDinle() {
    while (Serial.available() > 0) {
        uint8_t c = Serial.read();
        
        switch (c) {
            case SLIP_END:
                if (slipRxIndex > 0) {
                    slipUdp.beginPacket(IPAddress(127, 0, 0, 1), WG_ENDPOINT_PORT);
                    slipUdp.write(slipRxBuffer, slipRxIndex);
                    slipUdp.endPacket();
                    slipRxIndex = 0; 
                }
                break;
                
            case SLIP_ESC:
                if (Serial.available() > 0) {
                    uint8_t next_c = Serial.read();
                    if (next_c == SLIP_ESC_END) slipRxBuffer[slipRxIndex++] = SLIP_END;
                    else if (next_c == SLIP_ESC_ESC) slipRxBuffer[slipRxIndex++] = SLIP_ESC;
                }
                break;
                
            default:
                if (slipRxIndex < SLIP_BUFFER_SIZE) {
                    slipRxBuffer[slipRxIndex++] = c;
                }
                break;
        }
    }
}

bool ntpSenkronize() {
    configTime(3 * 3600, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");
    unsigned long t0 = millis();
    struct tm ti;
    while (!getLocalTime(&ti)) {
        if (millis() - t0 > NTP_TIMEOUT_MS) return false;
        delay(200);
    }
    return true;
}

// ─── SETUP ───────────────────────────────────────────────────────────────────

void setup() {
    auto cfg = M5.config();
    cfg.internal_imu = true;
    M5.begin(cfg);

    M5.Power.setExtOutput(true); 

    Serial.begin(115200);

    canvas.createSprite(M5.Display.width(), M5.Display.height() - 50);
    wifiYigininiSifirla();

    wifiTara();
}

// ─── LOOP ────────────────────────────────────────────────────────────────────

void loop() {
    if (durum == D_HATA) { delay(1000); return; }

    M5.update();

    // ── DURUM: AĞ SEÇİM EKRANI ──
    if (durum == D_AG_SECIM) {
        // Durum yeni değiştiyse sahte dokunmaları engellemek için 300ms guard koyuyoruz
        if (millis() - sonDurumDegisimZamani > 300 && M5.Touch.getCount() > 0) {
            auto t = M5.Touch.getDetail(0);
            if (t.wasPressed()) { tBaslangicY = t.y; tSonY = t.y; kaydiriliyor = false; }
            else if (t.isPressed()) {
                int deltaY = t.y - tSonY;
                if (abs(t.y - tBaslangicY) > 12) {
                    kaydiriliyor = true;
                    scrollY += deltaY;
                    if (scrollY > 60) scrollY = 60;
                    if (scrollY < maxScrollY) scrollY = maxScrollY;
                    agListesiRender();
                    tSonY = t.y;
                }
            }
            else if (t.wasReleased()) {
                if (kaydiriliyor && scrollY >= 45) { 
                    wifiTara(); 
                } 
                else if (!kaydiriliyor) { 
                    int idx = dokunulanIndexBul(t.y);
                    if (idx >= 0) {
                        secilenSSID = WiFi.SSID(idx);
                        int bi = bilinenAgBul(secilenSSID);
                        if (bi >= 0) {
                            girilenSifre = bilinenAglar[bi].password;
                            durum = D_WIFI_BAGLANIYOR;
                            sonDurumDegisimZamani = millis();
                        } else {
                            durum = D_KLAVYE_GIRIS;
                            sonDurumDegisimZamani = millis();
                            girilenSifre = ""; klavyeModu = K_KUCUK;
                            staticEkranCiz("Sifre Giriniz", "Sifrenizi girip BAGLAN butonuna basin");
                            klavyeArayuzCiz();
                        }
                    }
                }
                if(scrollY > 0 && durum == D_AG_SECIM) { scrollY = 0; agListesiRender(); }
                kaydiriliyor = false;
            }
        }
    }

    // ── DURUM: KLAVYE EKRANI ──
    else if (durum == D_KLAVYE_GIRIS) {
        if (millis() - sonDurumDegisimZamani > 300 && M5.Touch.getCount() > 0) {
            auto t = M5.Touch.getDetail(0);
            if (t.wasPressed()) klavyeDokunmaSorgu(t.x, t.y);
        }
    }

    // ── DURUM: WI-FI BAĞLANTI SÜRECİ ──
    // ── DURUM: WI-FI BAĞLANTI SÜRECİ ──
    else if (durum == D_WIFI_BAGLANIYOR) {
        staticEkranCiz("Wi-Fi Baglaniyor", "Gecis saglaniyor, lutfen bekleyin...", C_UYARI);
        
        WiFi.begin(secilenSSID.c_str(), girilenSifre.c_str());
        wifiBasladi = millis();

        while (WiFi.status() != WL_CONNECTED) {
            if (millis() - wifiBasladi > WIFI_TIMEOUT_MS) {
                staticEkranCiz("Baglanti Basarisiz", "Sifre yanlis veya Zaman asimi!", C_HATA);
                delay(2500);
                wifiTara();
                return;
            }
            delay(500);
        }

        staticEkranCiz("NTP Ayari", "Zaman sunucusu esitleniyor...", C_UYARI);
        
        // DOĞRU KULLANIM: Fonksiyonu burada sadece çağırıyoruz (invoke ediyoruz)
        if (!ntpSenkronize()) {
            staticEkranCiz("Zaman Hatasi", "NTP sunucusuna ulasilamadi!", C_HATA);
            delay(2000);
            wifiTara();
            return;
        }

        staticEkranCiz("WireGuard", "Kripto tünel protokolü tetikleniyor...", C_UYARI);
        
        wgAktif = wg.begin(WG_LOCAL_IP, WG_PRIVATE_KEY, WG_ENDPOINT_ADDRESS, WG_PUBLIC_KEY, WG_ENDPOINT_PORT);
        
        if (wgAktif) {
            durum = D_AKTIF;
            sonDurumDegisimZamani = millis();
            staticEkranCiz("MEA-Dongle GATEWAY", "Serial Baglantisi dinleniyor... Trafik Aktif.", C_OK);
            M5.Display.setTextColor(C_METIN);
            M5.Display.setTextSize(2);
            M5.Display.setCursor(10, 45); M5.Display.printf("SSID: %s\n", WiFi.SSID().c_str());
            M5.Display.setCursor(10, 75); M5.Display.printf("Tunel: %s\n", WG_LOCAL_IP.toString().c_str());
            M5.Display.setCursor(10, 105); M5.Display.printf("Mode: Serial SLIP Gateway\n");
        } else {
            staticEkranCiz("Tünel Hatası", "WireGuard el sikismasi basarisiz oldu!", C_HATA);
            durum = D_HATA;
            sonDurumDegisimZamani = millis();
        }
    }

    // ── DURUM: AKTİF TÜNEL ──
    else if (durum == D_AKTIF) {
        if (WiFi.status() != WL_CONNECTED) {
            durum = D_WIFI_BAGLANIYOR;
            sonDurumDegisimZamani = millis();
            return;
        }
        slipPaketleriniDinle();
    }

    delay(1); 
}