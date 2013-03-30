#!/bin/sh -e

cd $(dirname "$0")

make
mkdir lib
ldd main | perl -ne 'if(s{.*?(/usr/\S*).*}{$1}){print}' | xargs ln -st lib
tar czhf pack.tar.gz main makefile lib *.cc *.sh *.png
