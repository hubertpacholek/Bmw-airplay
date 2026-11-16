# AirPlay HP

Odbiornik AirPlay dla iDrive 7 (MGU): odbicie lustrzane ekranu iPhone'a na środkowym
ekranie, z dźwiękiem w głośnikach auta. Obraz tylko na postoju. Instaluje się ze
[Store](https://github.com/robertnisipeanu/mgu-store).

Oparte na **myAirPlay** z [aurelmarius/bmw-apps](https://github.com/aurelmarius/bmw-apps)
(historia zmian tych plików jest zachowana w tym repozytorium). Różnica: **bufor
wyrównujący** dla odbicia lustrzanego ekranu. Obraz i dźwięk idą według sygnatur czasu
iPhone'a, 200 ms po najszybszych pakietach, więc nierówne Wi-Fi nie powoduje przycinek w
filmach (`native/uxplay-mgu/README.md`, „The jitter buffer”).

## Instalacja

1. Jeśli masz **myAirPlay** od aurelmarius, usuń najpierw w Store aplikację i jej usługę:
   oba odbiorniki używają tych samych portów i tej samej nazwy dla iPhone'a.
2. W aucie: **Store → Settings → Add source** i adres
   `https://raw.githubusercontent.com/hubertpacholek/bmw-airplay/main/catalog.json`.
3. Zainstaluj **AirPlay HP** (usługa AirPlay HP instaluje się razem z nią).
4. iPhone w Wi-Fi auta: **Centrum sterowania → Odbicie lustrzane ekranu → BMW iDrive**.

## Budowanie

```sh
sh native/uxplay-mgu/build.sh      # odbiornik w Dockerze (ubuntu:18.04, glibc 2.27 jak w jednostce)
python3 tools/make_catalog.py      # aplikacja, usługa i catalog.json w packages/
for t in tests/*.js; do node "$t"; done
cc -O2 -pthread native/uxplay-mgu/renderer/test_jitter.c native/uxplay-mgu/renderer/mgu_jitter.c -lm -o /tmp/tj && /tmp/tj
```

## Licencja

GPL-3.0-or-later (`LICENSE`). Odbiornik to UxPlay ([FDH2/UxPlay](https://github.com/FDH2/UxPlay),
GPL-3.0), dekoder z [JustAnotherPU/moonride](https://github.com/JustAnotherPU/moonride)
(GPL-3.0), FFmpeg na LGPL-2.1 (`FFMPEG-LICENSE` w paczce usługi).
