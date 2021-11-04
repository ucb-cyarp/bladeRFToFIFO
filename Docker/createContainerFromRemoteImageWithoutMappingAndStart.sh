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

cd $scriptSrc
$scriptSrc/createLocalContainerImage.sh

#Create and run the container.  Map the project directory
echo "docker run -d --cap-add sys_ptrace -p127.0.0.1:2223:22 --name bladerf-to-fifo-clion_remote_env bladerf-to-fifo-remote-cpp-env-local:latest"
docker run -d --cap-add sys_ptrace -p127.0.0.1:2223:22 --name bladerf-to-fifo-clion_remote_env bladerf-to-fifo-remote-cpp-env-local:latest

#Set the mapped directiry to /project

#Reset the ssh known hosts entry
echo "ssh-keygen -f $HOME/.ssh/known_hosts -R [localhost]:2223"
ssh-keygen -f "$HOME/.ssh/known_hosts" -R "[localhost]:2223"

cd ${oldDir}