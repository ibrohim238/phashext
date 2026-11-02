# phashext

PHP-расширение с нативной математикой перцептивного хеша для пакета `ibrohim238/phash`. Других потребителей у него нет: пакет требует `ext-phashext`, а расширение повторяет арифметику эталона на Python (imagehash 4.3.1, Pillow 9.4.0, SciPy 1.10.0) бит в бит.

На PHP те же шаги были в 15–100 раз медленнее, поэтому вынесены в C++:

| Функция | Что делает |
|---|---|
| `phashext_luma` | RGB → яркость по ITU-R 601-2, как `PIL.Image.convert('L')` |
| `phashext_png_unfilter` | снятие построчных фильтров PNG (RFC 2083, 6) |
| `phashext_png_luma` | яркость строк 8-битного PNG |
| `phashext_resize` | Lanczos-ресайз из Pillow 9.4.0 `Resample.c` для режима L |
| `phashext_dct` | DCT-II через оригинальный pocketfft из SciPy |

Размеры из параметров сверяются с длиной переданных строк: несогласованный вызов получает исключение, а не выход за границу буфера. Проверяется тестом `PhashExtTest` в `ibrohim238/phash`.

## Файл расширения

В git бинарников нет, файл собирается командой `make`. Расширение привязано к ABI PHP, разрядности потоков и архитектуре, поэтому под каждую комбинацию нужен свой файл, например `phashext-<версия>-8.5-nts-x86_64.so` (PHP 8.5 NTS, x86_64, glibc). PHP-CPP влинкован статически, зависимости только libc и libm — на машине, где расширение подключается, ни компилятора, ни PHP-CPP не нужно.

Версия расширения задаётся в `src/ext.cpp` (`Php::Extension`) и попадает в `phpversion('phashext')`; по ней composer проверяет `ext-phashext` у `ibrohim238/phash`.

## Подключение

```bash
cp build/phashext-<версия>-8.5-nts-x86_64.so "$(php-config --extension-dir)/phashext.so"
echo 'extension=phashext.so' > "$(php-config --ini-dir)/99-phashext.ini"
php -r 'echo phpversion("phashext");'
```

Подключать файл нужно до `composer install`, чтобы composer увидел расширение.

## Сборка

Внутри образа `php:<ветка>-fpm` нужной версии PHP (версия образа задаёт версию PHP в имени файла), с установленным git:

```bash
make phpcpp  # клонирует PHP-CPP v2.4.16 с GitHub, собирает и ставит
make         # build/phashext-<версия>-<php>-<nts|ts>-<arch>.so
make check   # расширение загружается и отдаёт версию из src/ext.cpp
```

Флаг `-ffp-contract=off` в `Makefile` обязателен: он запрещает FMA, иначе порядок округлений разойдётся с эталоном и хеши перестанут совпадать.

Компилятор именно `g++`: DCT берётся из **оригинального pocketfft** — `src/scipy/pocketfft_hdronly.h`, дословная копия `scipy/fft/_pocketfft/pocketfft_hdronly.h` из SciPy v1.10.0. Это тот же код, которым считает эталон, поэтому совпадение получается по построению.

## Проверка

Тесты расширения живут в `ibrohim238/phash`: `PhashCalculatorTest` сверяет хеши с эталоном на фикстурах, `PhashExtTest` проверяет отказ на несогласованных размерах. Запуск с собранным файлом:

```bash
cd ../phash-php
php -d extension=/path/to/phashext/build/phashext-<версия>-8.5-nts-x86_64.so vendor/bin/phpunit
```
