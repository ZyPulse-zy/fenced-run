include $(TOPDIR)/rules.mk

PKG_NAME:=fenced-run
PKG_VERSION:=0.1.0
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0
PKG_LICENSE_FILES:=COPYING
PKG_MAINTAINER:=Yuen Su <zypulsecore@gmail.com>

include $(INCLUDE_DIR)/package.mk

define Package/fenced-run
  SECTION:=net
  CATEGORY:=Network
  TITLE:=Bounded command execution with inherited writer exclusion
endef

define Package/fenced-run/description
 Run finite commands with bounded output and deadlines, inherited advisory
 exclusion, and confirmed cleanup of owned descendants. No service is installed.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/fenced-run.c ./COPYING $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CPPFLAGS) $(TARGET_CFLAGS) -std=c11 \
		-Wall -Wextra -Werror $(PKG_BUILD_DIR)/fenced-run.c \
		$(TARGET_LDFLAGS) -o $(PKG_BUILD_DIR)/fenced-run
endef

define Package/fenced-run/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/fenced-run $(1)/usr/bin/
endef

$(eval $(call BuildPackage,fenced-run))
