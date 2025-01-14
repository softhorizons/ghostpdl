#!/bin/bash

echo "Build mtdemo"

cd ../gsjava

bash build_darwin.sh

cd ../mtdemo

cp ../mtdemo/gsjava.jar gsjava.jar

echo "Compiling Java source..."
javac -classpath "../gsjava/bin:." "Main.java" "Worker.java"
echo "Done."
