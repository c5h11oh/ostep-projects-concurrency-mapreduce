#!/bin/bash
./mapreduce-executable testcases/*.txt
cd testresult
OUT=$(find . -name 'out*')
for o in $OUT; do
    # echo $o
    sort $o > "sorted/${o%.txt}-sorted.txt"
done
cd sorted
mapfile -d '' FILE < <(find . -name "out*" -print0)
let LIMIT=${#FILE[@]}-1
for ((i = 0 ; i < $LIMIT ; i++ )); do
    let j=$i+1
    diff -s ${FILE[$i]} ${FILE[$j]} &>>"diff.txt"
done
cd ../..