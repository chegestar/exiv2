#!/bin/sh
# Create a module that depends on all MakerNote subclasses to
# force initialisation of static data in the corresponding 
# components when using the static library.
rm -f mn.cpp
echo "// Generated by mn.sh on" `date` "- do not edit" > mn.cpp
for file in *mn.hpp; do
    echo "#include \""$file"\"" >> mn.cpp
done
echo "namespace {" >> mn.cpp
for file in *mn.hpp; do
    class=`grep 'class .*MakerNote.*:' $file | awk '{print $2}'`
    echo "    Exiv2::"$class `basename $file .hpp`";" >> mn.cpp
done
echo "}" >> mn.cpp
