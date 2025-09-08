FROM docker.io/gcc:15 AS build
WORKDIR /app
COPY ./c64.c ./*.h .
ENV CFLAGS="-Wall -Ofast -march=x86-64-v2 -funroll-loops -fwhole-program -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables -fweb -fipa-pta -fgcse-sm -fgcse-las -fdata-sections -ffunction-sections -s -static -Wl,--gc-sections -Wl,--strip-all -Wl,-z,norelro -Wl,--build-id=none -Wl,-O1"
RUN localedef --delete-from-archive `localedef --list-archive` && \
    localedef --add-to-archive /usr/lib/locale/C.utf8
RUN gcc c64.c -o c64 $CFLAGS -lncursesw -ltinfo

# don't need all the build stuff for the final image
FROM scratch

# prepare terminfo
ENV TERM=xterm-256color TERMINFO=/usr/share/terminfo I18NPATH=/usr/lib/locale COLORTERM=truecolor
COPY --from=build /usr/share/terminfo/x/xterm-256color /usr/share/terminfo/x/xterm-256color
COPY --from=build /usr/lib/locale/C.utf8/* /usr/lib/locale/C.utf8/
COPY --from=build /usr/lib/locale/locale-archive /usr/lib/locale/

# prepare c64 binary
COPY --from=build /app/c64 /
ENTRYPOINT [ "/c64" ]
