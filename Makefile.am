bin_PROGRAMS = kterm
kterm_SOURCES = confedit.c icons.c keyboard.c keyseq.c kterm.c panel.c connpage.c parse_config.c parse_layout.c refresh.c settings.c shortbar.c shortcuts.c shortpage.c sshconf.c
if KINDLE
kterm_SOURCES += flash.c kindle.c ime.c voice.c voiceserver.c voicebar.c notice.c remote.c tmux.c tmuxpick.c imageview.c bellbar.c touch.c qrcodegen.c
# finds machines of the local network for ssh, see kterm-mdns.c
bin_PROGRAMS += kterm-mdns
kterm_mdns_SOURCES = kterm-mdns.c
kterm_mdns_LDADD =
endif

icon_svgs = $(wildcard $(srcdir)/assets/icons/*.svg)
icon_pngs = $(patsubst $(srcdir)/assets/icons/%.svg,generated-icons/%.png,$(icon_svgs)) \
	generated-icons/voice-64.png generated-icons/voice-180.png
images = $(icon_svgs) $(wildcard $(srcdir)/assets/*.svg)
EXTRA_DIST = assets/images.gresource.xml $(images)
nodist_kterm_SOURCES = image-resources.c
CLEANFILES = image-resources.c $(icon_pngs)

generated-icons/%.png: $(srcdir)/assets/icons/%.svg
	$(AM_V_GEN)$(MKDIR_P) generated-icons && $(RSVG_CONVERT) -o "$@" "$<"

generated-icons/voice-%.png: $(srcdir)/assets/voice-favicon.svg
	$(AM_V_GEN)$(MKDIR_P) generated-icons && $(RSVG_CONVERT) -w "$*" -h "$*" -o "$@" "$<"

image-resources.c: $(srcdir)/assets/images.gresource.xml $(images) $(icon_pngs)
	$(AM_V_GEN)$(GLIB_COMPILE_RESOURCES) --sourcedir="$(srcdir)/assets" --sourcedir=. --generate-source --target="$@" "$<"

ktermconfdir = $(sysconfdir)/kterm
ktermconf_DATA = kterm.conf shortcuts.conf
layoutsdir = $(ktermconfdir)/layouts
layouts_DATA = layouts/keyboard.xml
layoutsimgdir = $(layoutsdir)/img
layoutsimg_DATA = layouts/img/*.png

kterm_CFLAGS = @GTK_CFLAGS@ @VTE_CFLAGS@ @GDK_X11_CFLAGS@ @DBUS_CFLAGS@ -DSYSCONFDIR=\"$(sysconfdir)\"
kterm_LDADD = @GTK_LIBS@ @VTE_LIBS@ @GDK_X11_LIBS@ @DBUS_LIBS@

AM_CPPFLAGS = -pedantic -Wall -Wextra

kterm_pkg = $(abs_builddir)/kterm-kindle-$(VERSION).zip

dist-kindle: kterm
	rm -f $(kterm_pkg)
	KTERM_TMPDIR=`mktemp -d` \
	&& mkdir $${KTERM_TMPDIR}/kterm \
	&& cp -RL kindle.pkg/* $${KTERM_TMPDIR}/kterm \
	&& cp kterm $${KTERM_TMPDIR}/kterm/bin \
	&& cd $${KTERM_TMPDIR} \
	&& sed -i'.bak' 's/@VER@/$(VERSION)/' kterm/config.xml \
	&& rm kterm/config.xml.bak \
	&& zip -qr $(kterm_pkg) kterm \
	&& cd - \
	&& rm -rf $${KTERM_TMPDIR}

distclean-local:
	-rm -f $(kterm_pkg)
