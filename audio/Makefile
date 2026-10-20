LIBUSB ?= $(shell brew --prefix libusb 2>/dev/null || echo /opt/homebrew/opt/libusb)
CC = clang
CFLAGS = -std=c11 -O2 -g -Wall -Wextra -I$(LIBUSB)/include/libusb-1.0
LDLIBS = $(LIBUSB)/lib/libusb-1.0.a -framework IOKit -framework CoreFoundation -framework Security -framework AudioToolbox -framework CoreAudio

BUILD = build
TOOLS = probe rawcap rec tone monitor
USB_OBJ = $(BUILD)/podx3_usb.o

DRIVER = $(BUILD)/PodX3Audio.driver

all: $(addprefix $(BUILD)/,$(TOOLS)) $(BUILD)/podx3d $(DRIVER)

$(BUILD):
	mkdir -p $@

$(USB_OBJ): usb/podx3_usb.c usb/podx3_usb.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%: tools/%.c $(USB_OBJ) usb/podx3_usb.h common/*.h | $(BUILD)
	$(CC) $(CFLAGS) $< $(USB_OBJ) $(LDLIBS) -o $@

# USB helper daemon (packaging/com.pkalamarski.podx3d.plist runs this binary in place)
$(BUILD)/podx3d: helper/podx3d.c helper/rt.h $(USB_OBJ) usb/podx3_usb.h common/*.h | $(BUILD)
	$(CC) $(CFLAGS) $< $(USB_OBJ) $(LDLIBS) -o $@

# HAL plug-in bundle, ad-hoc signed (coreaudiod refuses unsigned plug-ins)
$(DRIVER): driver/PodX3Audio.c common/*.h packaging/Info.plist | $(BUILD)
	mkdir -p $@/Contents/MacOS
	$(CC) $(CFLAGS) -bundle $< -framework CoreAudio -framework CoreFoundation -o $@/Contents/MacOS/PodX3Audio
	cp packaging/Info.plist $@/Contents/Info.plist
	codesign -s - --force $@
	touch $@

# Hardware-free tests: DLL, ring math, plug-in clock, RT thread lookup, plug-in properties
$(BUILD)/test_logic: tests/test_logic.c common/*.h helper/rt.h | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/test_plugin: tests/test_plugin.c | $(BUILD)
	$(CC) $(CFLAGS) $< -framework CoreAudio -framework CoreFoundation -o $@

test: $(BUILD)/test_logic $(BUILD)/test_plugin $(DRIVER)
	$(BUILD)/test_logic
	$(BUILD)/test_plugin $(DRIVER)
	plutil -lint packaging/Info.plist packaging/com.pkalamarski.podx3d.plist
	codesign -v $(DRIVER)

clean:
	rm -rf $(BUILD)

.PHONY: all clean test
