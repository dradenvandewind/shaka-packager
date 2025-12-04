FROM alpine:3.19 as submodules

# Install git seulement pour ce stage
RUN apk add --no-cache git

#for local build
##WORKDIR /shaka-packager
##COPY . .
#RUN git submodule update --init --recursive

# Initialiser les submodules une seule fois dans un stage séparé
RUN git clone https://github.com/dradenvandewind/shaka-packager.git && \
cd shaka-packager && \
git checkout vvc_integration && \
git submodule update --init --recursive




FROM alpine:3.19 as builder

# Install utilities, libraries, and dev tools.
RUN apk add --no-cache \
        bash curl \
        bsd-compat-headers linux-headers \
        build-base cmake git ninja python3 curl bash unzip vim nano gdb

WORKDIR /shaka-packager

# Copier seulement les sources avec les submodules déjà initialisés
COPY --from=submodules /shaka-packager .

# Étape de build sans avoir à refaire les submodules
RUN rm -rf build && \
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -G Ninja
RUN  cmake --build build/ --config Debug --parallel

# Copy only result binaries to our final image.
FROM alpine:3.19
RUN apk add --no-cache libstdc++ python3
COPY --from=builder /shaka-packager/build/packager/packager \
                    /shaka-packager/build/packager/mpd_generator \
                    /shaka-packager/build/packager/pssh-box.py \
                    /usr/bin/

# Copy pyproto directory, which is needed by pssh-box.py script.
COPY --from=builder /shaka-packager/build/packager/pssh-box-protos \
                    /usr/bin/pssh-box-protos

                    COPY --from=builder /shaka-packager/build/packager/*.266 /home/
COPY --from=builder /shaka-packager/*.sh /home/
COPY --from=builder /shaka-packager/*.ts /home/
COPY --from=builder /shaka-packager/*.py /home/

WORKDIR /home

RUN packager --version && mpd_generator --version

CMD ["/bin/bash"]