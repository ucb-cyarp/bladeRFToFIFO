#!/bin/bash

oldDir=$(pwd)

#Get build dir
scriptSrc=$(dirname "${BASH_SOURCE[0]}")
cd $scriptSrc
scriptSrc=$(pwd)
if [[ $(basename $scriptSrc) == Docker ]]; then
    cd ..
    projectDir=$(pwd)
else
    echo "Error: Unable to determine location of project directory"
    cd $oldDir
    exit 1
fi

#Clean old container and images 
echo "docker stop bladerf-to-fifo-clion_remote_env""
docker stop bladerf-to-fifo-clion_remote_env"

echo "docker rm bladerf-to-fifo-clion_remote_env""
docker rm bladerf-to-fifo-clion_remote_env"

# echo "docker rmi laminar-remote-cpp-env-base-local"
# docker rmi laminar-remote-cpp-env-base-local

timestamp=$(date +%F_%H-%M-%S)

#Build the container image
cd $scriptSrc
echo "docker build --no-cache --progress=plain -t bladerf-to-fifo-remote-cpp-env-base-local:${timestamp} -f Dockerfile.base . 2>&1 | tee dockerBuildBase.log"
docker build --no-cache --progress=plain -t "bladerf-to-fifo-remote-cpp-env-base-local:${timestamp}" -f Dockerfile.base . 2>&1 | tee dockerBuildBase.log

echo "docker tag bladerf-to-fifo-remote-cpp-env-base-local:${timestamp} bladerf-to-fifo-remote-cpp-env-base-local:latest"
docker tag "bladerf-to-fifo-remote-cpp-env-base-local:${timestamp}" bladerf-to-fifo-remote-cpp-env-base-local:latest

echo "${timestamp}" > lastBuiltTag.log

cd ${oldDir}
