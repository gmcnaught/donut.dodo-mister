FROM godot4-armhf-build:bullseye
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends bc flex bison git libssl-dev libelf-dev kmod cpio gcc libc6-dev && rm -rf /var/lib/apt/lists/*
