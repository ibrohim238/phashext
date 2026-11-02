# Сборка расширения phashext. Запускать внутри образа php:<версия>-fpm: сначала `make phpcpp`
# (клонирует с GitHub, собирает и ставит PHP-CPP), затем `make` (см. README).
#
# Имя файла фиксирует всё, к чему привязан бинарник: версию расширения, ветку PHP, разрядность потоков
# и архитектуру. Версия расширения берётся из Php::Extension в src/ext.cpp, остальное — из запущенного PHP.

VERSION     := $(shell sed -n 's/.*Php::Extension extension("phashext", "\([^"]*\)").*/\1/p' src/ext.cpp)
PHP_VERSION := $(shell php -r 'echo PHP_MAJOR_VERSION, ".", PHP_MINOR_VERSION;')
THREADS     := $(shell php -r 'echo ZEND_THREAD_SAFE ? "ts" : "nts";')
ARCH        := $(shell uname -m)
TARGET      := build/phashext-$(VERSION)-$(PHP_VERSION)-$(THREADS)-$(ARCH).so

CXX      ?= g++
# -ffp-contract=off запрещает FMA: иначе порядок округлений разойдётся с эталоном и хеши перестанут совпадать
CXXFLAGS ?= -O2 -ffp-contract=off -std=c++17 -fpic
PHPCPP   ?= /usr/lib/libphpcpp.a

.PHONY: all phpcpp check clean

all: $(TARGET)

# PHP-CPP клонируется с GitHub по тегу и ставится в /usr/lib и /usr/include
PHPCPP_VERSION ?= v2.4.16
PHPCPP_SRC     ?= /tmp/php-cpp

phpcpp:
	rm -rf $(PHPCPP_SRC)
	git clone --depth 1 --branch $(PHPCPP_VERSION) https://github.com/CopernicaMarketingSoftware/PHP-CPP.git $(PHPCPP_SRC)
	$(MAKE) -C $(PHPCPP_SRC) -j"$$(nproc)"
	$(MAKE) -C $(PHPCPP_SRC) install

$(TARGET): src/ext.cpp src/phash_math.cpp src/scipy/pocketfft_hdronly.h
	mkdir -p build
	$(CXX) $(CXXFLAGS) -shared -o $@ src/ext.cpp src/phash_math.cpp \
	    $$(php-config --includes) -static-libstdc++ -static-libgcc $(PHPCPP) -lm
	strip --strip-unneeded $@

# Расширение загружается и отдаёт ту версию, что зашита в исходнике
check: $(TARGET)
	test "$$(php -d extension=$(CURDIR)/$(TARGET) -r 'echo phpversion("phashext");')" = "$(VERSION)"

clean:
	rm -f $(TARGET)
