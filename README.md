# AirPlay HP

Odbiornik AirPlay dla iDrive 7 (MGU): odbicie lustrzane ekranu iPhone'a na środkowym
ekranie, z dźwiękiem w głośnikach auta. Obraz domyślnie tylko na postoju. Aplikacja jest po
polsku. Instaluje się ze [Store](https://github.com/robertnisipeanu/mgu-store).

Oparte na **myAirPlay** z [aurelmarius/bmw-apps](https://github.com/aurelmarius/bmw-apps)
(historia zmian tych plików jest zachowana w tym repozytorium). Różnica: **bufor
wyrównujący** dla odbicia lustrzanego ekranu. Obraz i dźwięk idą według sygnatur czasu
iPhone'a, 200 ms po najszybszych pakietach, więc nierówne Wi-Fi nie powoduje przycinek w
filmach (`native/uxplay-mgu/README.md`, „The jitter buffer”). Film przy odbiciu lustrzanym
pokazuje się bez swoich czarnych pasów, w swoich proporcjach albo powiększony na cały ekran (ustawienia
„Film na cały ekran” i „Film na ekranie”: odbiornik znajduje pasy, `native/uxplay-mgu/video/src/film_bars.cpp`). Opcjonalnie obraz także na HUD, z wyborem wielkości (domyślnie 40%) i położenia, przy których
ustawianiu HUD pokazuje obraz testowy (ustawienia „Obraz na HUD”:
wyłączony, tylko na postoju albo także w czasie jazdy, bo w aucie, dla którego powstała ta aplikacja, HUD
jest przeniesiony na stronę pasażera; wymaga **cluster-render** ze Store, ze źródła `robertnisipeanu/idrive-apps`).

To samo źródło daje też **Maps on cluster** z mapą CarPlay (np. Yanosika) na zegarach 1,3 raza bardziej
oddaloną: waze-on-cluster 0.14.19 z forka [hubertpacholek/yanosiknazegarach](https://github.com/hubertpacholek/yanosiknazegarach)
(`store/README.md` tam; tutaj `extra/components.json` i paczki w `packages/`).

## Instalacja

1. Jeśli masz **myAirPlay** od aurelmarius, usuń najpierw w Store aplikację i jej usługę:
   oba odbiorniki używają tych samych portów i tej samej nazwy dla iPhone'a.
2. W aucie: **Store → Settings → Add source**, wpisz `hubertpacholek/Bmw-airplay` i wybierz
   **Releases**. (Działa też adres `https://raw.githubusercontent.com/hubertpacholek/Bmw-airplay/main/catalog.json`.)
3. Zainstaluj **AirPlay HP** (usługa AirPlay HP instaluje się razem z nią).
4. iPhone w Wi-Fi auta: **Centrum sterowania → Odbicie lustrzane ekranu → BMW iDrive**.

## Budowanie

```sh
sh native/uxplay-mgu/build.sh      # odbiornik w Dockerze (ubuntu:18.04, glibc 2.27 jak w jednostce)
python3 tools/make_catalog.py      # aplikacja, usługa i catalog.json w packages/
                                   # po wysłaniu na main workflow „Catalog release” aktualizuje wydanie `catalog`
for t in tests/*.js; do node "$t"; done
cc -O2 -pthread native/uxplay-mgu/renderer/test_jitter.c native/uxplay-mgu/renderer/mgu_jitter.c -lm -o /tmp/tj && /tmp/tj
```

## Licencja

GPL-3.0-or-later (`LICENSE`). Odbiornik to UxPlay ([FDH2/UxPlay](https://github.com/FDH2/UxPlay),
GPL-3.0), dekoder z [JustAnotherPU/moonride](https://github.com/JustAnotherPU/moonride)
(GPL-3.0), FFmpeg na LGPL-2.1 (`FFMPEG-LICENSE` w paczce usługi). `apps/hp_airplay/cluster_render_client.js`
to klient cluster-render z [robertnisipeanu/idrive-apps](https://github.com/robertnisipeanu/idrive-apps)
(`native/cluster-render/js`, commit 74e749d), na MPL-2.0, bez zmian.
