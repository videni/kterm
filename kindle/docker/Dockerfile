# Build environment for kterm on Kindle: Alpine armv7 with GTK+ 3 and VTE.
# Built once by kindle/build.sh; the packages are not downloaded per build.
FROM alpine:3.24
ARG APK_MIRROR=https://dl-cdn.alpinelinux.org/alpine
RUN sed -i "s|https://dl-cdn.alpinelinux.org/alpine|${APK_MIRROR}|" /etc/apk/repositories \
 && apk add --no-cache build-base autoconf automake libtool pkgconf gtk+3.0-dev vte3-dev
