#
# Makefile - Brother QL CUPS driver (ARM64)
#
# Requirements:
#   - Xcode Command Line Tools  (provides cc, clang)
#   - CUPS headers              (included with macOS SDK)
#
# Build:   make
# Install: sudo make install
# Package: make pkg
#

CC      = cc
CFLAGS  = -O2 -arch arm64 -Wall -Wextra \
          -I/usr/include \
          -I$(shell xcrun --show-sdk-path)/usr/include
LDFLAGS = -arch arm64 -lcups -L/usr/lib

FILTER_NAME = rastertoql
SRCS        = src/rastertoql.c
HDRS        = src/ql_protocol.h

# Installation paths
FILTER_DIR  = /usr/libexec/cups/filter
PPD_DIR     = /Library/Printers/PPDs/Contents/Resources

# Package output
PKG_DIR     = build/pkg
PKG_ROOT    = build/pkg-root
PKG_ID      = uk.co.eplop.cups.ql-driver
PKG_VERSION = 1.0
PKG_OUT     = build/QL-Driver-ARM64-$(PKG_VERSION).pkg

.PHONY: all clean install uninstall pkg

all: build/$(FILTER_NAME)

build/$(FILTER_NAME): $(SRCS) $(HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)
	@echo "Built $@ (arm64)"
	@file $@ | grep -q arm64 && echo "✓ Confirmed arm64 binary" || echo "WARNING: not arm64!"

clean:
	rm -rf build

install: build/$(FILTER_NAME)
	@echo "Installing filter to $(FILTER_DIR)..."
	install -d $(FILTER_DIR)
	install -m 755 -o root -g wheel build/$(FILTER_NAME) $(FILTER_DIR)/$(FILTER_NAME)
	@echo "Installing PPDs to $(PPD_DIR)..."
	install -d $(PPD_DIR)
	gzip -c ppd/QL-1100.ppd > $(PPD_DIR)/QL-1100.ppd.gz
	gzip -c ppd/QL-800.ppd  > $(PPD_DIR)/QL-800.ppd.gz
	@echo "Restarting CUPS..."
	launchctl stop org.cups.cupsd || true
	launchctl start org.cups.cupsd || true
	@echo ""
	@echo "✓ Installation complete."
	@echo "  Add your printer via System Settings → Printers & Scanners"
	@echo "  Select 'Brother QL-1100 (ARM64)' or 'Brother QL-800 (ARM64)'"

uninstall:
	rm -f $(FILTER_DIR)/$(FILTER_NAME)
	rm -f $(PPD_DIR)/QL-1100.ppd.gz
	rm -f $(PPD_DIR)/QL-800.ppd.gz
	launchctl stop org.cups.cupsd || true
	launchctl start org.cups.cupsd || true
	@echo "✓ Uninstalled."

#
# Build a flat .pkg installer (no signing — for personal use)
# For distribution you would add: --sign "Developer ID Installer: ..."
#
pkg: build/$(FILTER_NAME)
	@echo "Building package..."
	@rm -rf $(PKG_ROOT) $(PKG_DIR)
	@mkdir -p $(PKG_ROOT)$(FILTER_DIR)
	@mkdir -p $(PKG_ROOT)$(PPD_DIR)
	@mkdir -p $(PKG_DIR)/scripts

	# Filter binary
	cp build/$(FILTER_NAME) $(PKG_ROOT)$(FILTER_DIR)/$(FILTER_NAME)
	chmod 755 $(PKG_ROOT)$(FILTER_DIR)/$(FILTER_NAME)

	# Compressed PPDs
	gzip -c ppd/QL-1100.ppd > $(PKG_ROOT)$(PPD_DIR)/QL-1100.ppd.gz
	gzip -c ppd/QL-800.ppd  > $(PKG_ROOT)$(PPD_DIR)/QL-800.ppd.gz

	# Postinstall script
	cp pkg/postinstall $(PKG_DIR)/scripts/postinstall
	chmod 755 $(PKG_DIR)/scripts/postinstall

	# Build component package
	pkgbuild \
		--root $(PKG_ROOT) \
		--scripts $(PKG_DIR)/scripts \
		--identifier $(PKG_ID) \
		--version $(PKG_VERSION) \
		--ownership recommended \
		$(PKG_OUT)

	@echo ""
	@echo "✓ Package built: $(PKG_OUT)"
	@echo "  Install with: sudo installer -pkg $(PKG_OUT) -target /"
