#/bin/bash

echo "Test group"
prterun -n 4 ../../examples/group >& /dev/null

echo "Test group_lcl_cid"
prterun -n 4 ../../examples/group_lcl_cid >& /dev/null

