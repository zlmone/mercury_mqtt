OPTIONS = -std=c99  -lpthread -lz -lm -lrt -lssl -lcrypto -ldl -g -pipe -Wp,-D_FORTIFY_SOURCE=2 -fexceptions -fstack-protector --param=ssp-buffer-size=4 -m64 -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE -fno-strict-aliasing -fwrapv -fPIC  -fPIC -g -static-libgcc -fno-omit-frame-pointer -fno-strict-aliasing -DMY_PTHREAD_FASTMUTEX=1

mercury236mqtt: mercury236mqtt.c util.c 
	$(CC) $^ $(OPTIONS) -o $@ -lmosquitto
clean:
	rm mercury236mqtt
