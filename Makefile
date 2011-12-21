src = src/log.c src/http_prox.c src/http_parser.c src/http_str.c \
    src/hash.c src/sock_easy.c src/http_conn.c common/time.c src/prox_bd.c
ldflags = "-Llthread"
includes = "-I./"
gccflags = "-g"

http_proxy : $(src)
	gcc -O3 $(gccflags) -Wall -lm -llthread $(includes) $(ldflags) $(src) -o http_proxy

clean:
	rm -f http_proxy *.o *.so
