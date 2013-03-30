#!/bin/sh -e

cd $(dirname "$0")

while read file date other
do mplayer -noconsolecontrols -osdlevel 2 "$file" -ss "$date" -title "$file: $date-$other"
done <results.txt
