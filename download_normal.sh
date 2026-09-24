#!/bin/bash
cat $1 |while read line
do
file=$(echo $line | sed 's/\r//')
wget -P $2 $file
done
