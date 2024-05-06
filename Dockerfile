FROM docker.io/gcc:13 AS build
COPY ./c64.c ./*.h .
ENV GCCFLAGS="-O3 -fwhole-program -fweb -fdata-sections -ffunction-sections -s -static -Wl,--gc-sections -Wl,--strip-all -Wl,-z,norelro -Wl,--build-id=none -Wl,-O1"
RUN gcc c64.c -o c64 $GCCFLAGS -lncursesw -ltinfo
RUN localedef --delete-from-archive `localedef --list-archive` && \
    localedef --add-to-archive /usr/lib/locale/C.utf8

# don't need all the build stuff for the final image
FROM scratch

# prepare terminfo
ENV TERM=xterm-256color TERMINFO=/usr/lib/terminfo I18NPATH=/usr/lib/locale
COPY --from=build /usr/lib/terminfo/x/xterm-256color /usr/lib/terminfo/x/xterm-256color
COPY --from=build /usr/lib/locale/C.utf8/* /usr/lib/locale/C.utf8/
COPY --from=build /usr/lib/locale/locale-archive /usr/lib/locale/

# prepare c64 binary
COPY --from=build c64 /
ENTRYPOINT [ "/c64" ]
