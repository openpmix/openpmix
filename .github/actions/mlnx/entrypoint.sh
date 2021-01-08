#!/bin/bash -xeE

rel_path=$(dirname $0)
abs_path=$(readlink -f $rel_path)

if [ "$1" = "build" ]; then
    jenkins_test_build="yes"
    jenkins_test_check="no"
    jenkins_test_src_rpm="no"
elif [ "$1" = "srcrpm" ]; then
    jenkins_test_build="no"
    jenkins_test_check="no"
    jenkins_test_src_rpm="yes"
elif [ "$1" = "test" ]; then
    jenkins_test_build="no"
    jenkins_test_check="yes"
    jenkins_test_src_rpm="no"
fi

jenkins_test_build=${jenkins_test_build:="yes"}
jenkins_test_check=${jenkins_test_check:="yes"}
jenkins_test_src_rpm=${jenkins_test_src_rpm:="yes"}
jenkins_test_vg=${jenkins_test_vg:="no"}

timeout_exe=${timout_exe:="timeout -s SIGKILL 1m"}

# prepare to run from command line w/o jenkins
if [ -z "$WORKSPACE" ]; then
    WORKSPACE=$PWD
    JOB_URL=$WORKSPACE
    BUILD_NUMBER=1
    JENKINS_RUN_TESTS=1
    NOJENKINS=${NOJENKINS:="yes"}
fi

OUTDIR=$WORKSPACE/out

prefix=jenkins
rm -rf ${WORKSPACE}/${prefix}
mkdir -p ${WORKSPACE}/${prefix}
work_dir=${WORKSPACE}/${prefix}
build_dir=${work_dir}/build
pmix_dir=${work_dir}/install
build_dir=${work_dir}/build
rpm_dir=${work_dir}/rpms
tarball_dir=${work_dir}/tarball

pmix_ver=`cat VERSION | grep "^major=" | cut -d "=" -f 2``cat VERSION | grep "^minor=" | cut -d "=" -f 2`

make_opt="-j$(nproc)"

test_ret=0

echo Running following tests:
set|grep jenkins_test_

function on_start()
{
    echo Starting on host: $(hostname)

    export distro_name=`python -c 'import platform ; print platform.dist()[0]' | tr '[:upper:]' '[:lower:]'`
    export distro_ver=`python  -c 'import platform ; print platform.dist()[1]' | tr '[:upper:]' '[:lower:]'`
    if [ "$distro_name" == "suse" ]; then
        patch_level=$(egrep PATCHLEVEL /etc/SuSE-release|cut -f2 -d=|sed -e "s/ //g")
        if [ -n "$patch_level" ]; then
            export distro_ver="${distro_ver}.${patch_level}"
        fi
    fi
    echo $distro_name -- $distro_ver

    # save current environment to support debugging
#    set +x
#    env| sed -ne "s/\(\w*\)=\(.*\)\$/export \1='\2'/p" > $WORKSPACE/test_env.sh
#    chmod 755 $WORKSPACE/test_env.sh
#    set -x
}

function on_exit
{
    set +x
    rc=$((rc + $?))
    echo exit code=$rc
    if [ $rc -ne 0 ]; then
        # FIX: when rpmbuild fails, it leaves folders w/o any permissions even for owner
        # jenkins fails to remove such and fails
        find $rpm_dir -type d -exec chmod +x {} \;
    fi
}

function check_out()
{
    local ret=0
    for out in `ls $OUTDIR/out.*`; do
        if [ "$pmix_ver" -ge 31 ]; then
            status=`cat $out | awk '{print $2}'`
        else
            status=`cat $out`
        fi
        if [ "$status" != "OK" ]; then
            ret=1
        fi
    done
    echo $ret
}

# $1 - test name
# $2 - test command
function check_result()
{
    set +e
    eval $timeout_exe $2
    ret=$?
    set -e
    client_ret=$(check_out)
    echo client_ret $client_ret
    if [ $ret -gt 0 ] || [ $client_ret -gt 0 ]; then
        echo "not ok $test_id $1 ($2)" >> $run_tap
        test_ret=1
    else
        echo "ok $test_id $1 ($2)" >> $run_tap
    fi
    rm $OUTDIR/*
    test_id=$((test_id+1))
}

function pmix_run_tests()
{
    cd $build_dir/test

    echo "1..14" >> $run_tap

    test_ret=0

    test_id=1
    # 1 blocking fence with data exchange among all processes from two namespaces:
    if [ "$pmix_ver" -ge 12 ]; then
        test_exec='./pmix_test -n 4 --ns-dist 3:1 --fence "[db | 0:0-2;1:0]" -o $OUTDIR/out'
        # All nspaces should started from 0 rank.                         ^ here is 0 rank for the second nspace
        check_result "blocking fence w/ data all" "$test_exec"
        test_exec='./pmix_test -n 4 --ns-dist 3:1 --fence "[db | 0:;1:0]" -o $OUTDIR/out'
        check_result "blocking fence w/ data all" "$test_exec"
    else
        # For old test version the rank counter for the second nspace may starts with not 0,
        # this case supports old versions
        test_exec='./pmix_test -n 4 --ns-dist 3:1 --fence "[db | 0:0-2;1:3]" -o $OUTDIR/out'
        check_result "blocking fence w/ data all" "$test_exec"
        test_exec='./pmix_test -n 4 --ns-dist 3:1 --fence "[db | 0:;1:3]" -o $OUTDIR/out'
        check_result "blocking fence w/ data all" "$test_exec"
    fi
    test_exec='./pmix_test -n 4 --ns-dist 3:1 --fence "[db | 0:;1:]" -o $OUTDIR/out'
    check_result "blocking fence w/ data all" "$test_exec"

    # 1 non-blocking fence without data exchange among processes from the 1st namespace
    test_exec='./pmix_test -n 4 --ns-dist 3:1 --fence "[0:]" -o $OUTDIR/out'
    check_result "non-blocking fence w/o data" "$test_exec"

    # blocking fence without data exchange among processes from the 1st namespace
    test_exec='./pmix_test -n 4 --ns-dist 3:1 --fence "[b | 0:]" -o $OUTDIR/out'
    check_result "blocking fence w/ data" "$test_exec"

    # non-blocking fence with data exchange among processes from the 1st namespace. Ranks 0, 1 from ns 0 are sleeping for 2 sec before doing fence test.
    test_exec='./pmix_test -n 4 --ns-dist 3:1 --fence "[d | 0:]" --noise "[0:0,1]" -o $OUTDIR/out'
    check_result "non-blocking fence w/ data" "$test_exec"

    # blocking fence with data exchange across processes from the same namespace.
    test_exec='./pmix_test -n 4 --job-fence -c -o $OUTDIR/out'
    check_result "blocking fence w/ data on the same nspace" "$test_exec"

    # blocking fence with data exchange across processes from the same namespace.
    test_exec='./pmix_test -n 4 --job-fence -o $OUTDIR/out'
    check_result "blocking fence w/o data on the same nspace" "$test_exec"

    # 3 fences: 1 - non-blocking without data exchange across processes from ns 0,
    # 2 - non-blocking across processes 0 and 1 from ns 0 and process 3 from ns 1,
    # 3 - blocking with data exchange across processes from their own namespace.
    # Disabled as incorrect at the moment
    # test_exec='./pmix_test -n 4 --job-fence -c --fence "[0:][d|0:0-1;1:]" --use-same-keys --ns-dist "3:1"'
    # check_result "mix fence" $test_exec

    # test publish/lookup/unpublish functionality.
    test_exec='./pmix_test -n 2 --test-publish -o $OUTDIR/out'
    check_result "publish" "$test_exec"

    # test spawn functionality.
    test_exec='./pmix_test -n 2 --test-spawn -o $OUTDIR/out'
    check_result "spawn" "$test_exec"

    # test connect/disconnect between processes from the same namespace.
    test_exec='./pmix_test -n 2 --test-connect -o $OUTDIR/out'
    check_result "connect" "$test_exec"

    # resolve peers from different namespaces.
    test_exec='./pmix_test -n 5 --test-resolve-peers --ns-dist "1:2:2" -o $OUTDIR/out'
    check_result "resolve peers" "$test_exec"


    if [ "$pmix_ver" -gt 11 ]; then
        # resolve peers from different namespaces.
        test_exec='./pmix_test -n 5 --test-replace 100:0,1,10,50,99 -o $OUTDIR/out'
        check_result "key replacement" "$test_exec"

        # resolve peers from different namespaces.
        test_exec='./pmix_test -n 5 --test-internal 10 -o $OUTDIR/out'
        check_result "local store" "$test_exec"
    fi
#    if [ "$pmix_ver" -ge 40 ]; then
#        # test direct modex
#        test_exec='./pmix_test -s 2 -n 2 --job-fence -o $OUTDIR/out'
#        check_result "direct modex" "$test_exec"

        # test full modex
#        test_exec='./pmix_test -s 2 -n 2 --job-fence -c -o $OUTDIR/out'
#        check_result "full modex" "$test_exec"
#    fi

    # run valgrind
    if [ "$jenkins_test_vg" = "yes" ]; then
        set +e
        module load tools/valgrind
        vg_opt="--tool=memcheck --leak-check=full --error-exitcode=0 --trace-children=yes  --trace-children-skip=*/sed,*/collect2,*/gcc,*/cat,*/rm,*/ls --track-origins=yes --xml=yes --xml-file=valgrind%p.xml --fair-sched=try --gen-suppressions=all"
        valgrind $vg_opt  ./pmix_test -n 4 --timeout 60 --ns-dist 3:1 --fence "[db | 0:;1:3]"
        valgrind $vg_opt  ./pmix_test -n 4 --timeout 60 --job-fence -c
        valgrind $vg_opt  ./pmix_test -n 2 --timeout 60 --test-publish
        valgrind $vg_opt  ./pmix_test -n 2 --timeout 60 --test-spawn
        valgrind $vg_opt  ./pmix_test -n 2 --timeout 60 --test-connect
        valgrind $vg_opt  ./pmix_test -n 5 --timeout 60 --test-resolve-peers --ns-dist "1:2:2"
        valgrind $vg_opt  ./pmix_test -n 5 --test-replace 100:0,1,10,50,99
        valgrind $vg_opt  ./pmix_test -n 5 --test-internal 10
        module unload tools/valgrind
        set -e
    fi

    if [ "$test_ret" = "0" ]; then
        echo "Test OK"
    else
        echo "Test failed"
    fi
}

trap "on_exit" INT TERM ILL KILL FPE SEGV ALRM

on_start

autogen_done=0
if [ -e ".autogen_done" ]; then
    autogen_done=1
fi

if [ -x "autogen.sh" ]; then
    autogen_script=./autogen.sh
else
    autogen_script=./autogen.pl
fi
configure_args=""

cd $WORKSPACE
if [ "$jenkins_test_build" = "yes" ]; then
    $autogen_script && touch .autogen_done
    echo ./configure --prefix=$pmix_dir $configure_args | bash -xeE
    make $make_opt install
    make $make_opt check || (cat test/test-suite.log && exit 12)
fi

cd $WORKSPACE
if [ "$jenkins_test_src_rpm" = "yes" ]; then
    echo "Checking for rpm ..."

    # check distclean
    make $make_opt distclean
    if [ "${autogen_done}" != "1" ]; then
        $autogen_script && touch .autogen_done
    fi

    if [ -x /usr/bin/dpkg-buildpackage ]; then
        echo "Do not support PMIX on debian"
    else
        echo ./configure --prefix=$pmix_dir $configure_args | bash -xeE || exit 11
        echo "Building PMIX src.rpm"
        rm -rf $tarball_dir
        mkdir -p $tarball_dir

        make_dist_args="--highok --distdir=$tarball_dir --greekonly"

        for arg in no-git-update dirtyok verok; do
            if grep $arg contrib/make_tarball 2>&1 > /dev/null; then
                make_dist_args="$make_dist_args --${arg}"
            fi
        done

        # ugly hack, make_tarball has hardcoded "-j32" and sometimes it fails on some race
        sed -i -e s,-j32,-j8,g contrib/make_tarball

        chmod +x ./contrib/make* ./contrib/buildrpm.sh
        echo contrib/make_tarball $make_dist_args | bash -xeE || exit 11

        # build src.rpm
        # svn_r=$(git rev-parse --short=7 HEAD| tr -d '\n') ./contrib/make_tarball --distdir=$tarball_dir
        tarball_src=$(ls -1 $tarball_dir/pmix-*.tar.bz2|sort -r|head -1)

        echo "Building PMIX bin.rpm"
        rpm_flags="--define 'mflags -j8' --define '_source_filedigest_algorithm md5' --define '_binary_filedigest_algorithm md5'"
        (cd ./contrib/ && env rpmbuild_options="$rpm_flags" rpmtopdir=$rpm_dir ./buildrpm.sh $tarball_src)
        # check distclean
        make $make_opt distclean
    fi
fi

cd $WORKSPACE
if [ "$jenkins_test_check" = "yes" ]; then
    run_tap=$WORKSPACE/run_test.tap
    rm -rf $run_tap

    export TMPDIR="/tmp"

    if [ ! -d "$OUTDIR" ]; then
        mkdir $OUTDIR
    fi

    # Run autogen only once
    if [ "${autogen_done}" != "1" ]; then
        $autogen_script && touch .autogen_done
    fi

    if [ $pmix_ver -le 20 ]; then
        # Test pmix/messaging
        echo "--------------------------- Building with messages ----------------------------------------"
        mkdir ${build_dir}
        cd ${build_dir}
        echo ${WORKSPACE}/configure --prefix=${pmix_dir} $configure_args --disable-visibility --disable-dstore | bash -xeE
        make $make_opt install
        echo "--------------------------- Checking with messages ----------------------------------------"
        echo "Checking without dstor:" >> $run_tap
        pmix_run_tests
        rm -Rf ${pmix_dir} ${build_dir}
        rc=$test_ret

        # Test pmix/dstore/flock
        echo "--------------------------- Building with dstore/flock ----------------------------------------"
        mkdir ${build_dir}
        cd ${build_dir}
        echo ${WORKSPACE}/configure --prefix=$pmix_dir $configure_args --disable-visibility --enable-dstore --disable-dstore-pthlck | bash -xeE
        make $make_opt install
        echo "--------------------------- Checking with dstore/flock ----------------------------------------"
        echo "Checking with dstor/flock:" >> $run_tap
        pmix_run_tests
        rm -Rf ${pmix_dir} ${build_dir}
        rc=$((test_ret+rc))

        # Test pmix/dstore/pthread-lock
        echo "--------------------------- Building with dstore/pthread-lock ----------------------------------------"
        mkdir ${build_dir}
        cd ${build_dir}
        echo ${WORKSPACE}/configure --prefix=$pmix_dir $configure_args --disable-visibility --enable-dstore | bash -xeE
        make $make_opt install
        echo "--------------------------- Checking with dstore/pthread-lock ----------------------------------------"
        echo "Checking with dstor:" >> $run_tap
        pmix_run_tests
        rm -Rf ${pmix_dir} ${build_dir}
        rc=$((test_ret+rc))
    else
        has_gds_ds21=0
        # Test pmix/dstore/pthread-lock
        echo "--------------------------- Building with dstore/pthread-lock ----------------------------------------"
        mkdir ${build_dir}
        cd ${build_dir}
        echo ${WORKSPACE}/configure --prefix=$pmix_dir $configure_args --disable-visibility | bash -xeE
        make $make_opt install
        echo "--------------------------- Checking with dstore/pthread-lock ----------------------------------------"
        echo "----dstore/pthread-lock----" >> $run_tap

        gds_list="hash ds12,hash"
        # check the existence of ds21 component
        has_gds_ds21=$($pmix_dir/bin/pmix_info --param gds ds21 | wc -l)
        if [ "$has_gds_ds21" -gt 0 ]; then
            gds_list="$gds_list ds21,hash"
        fi

        for gds in $gds_list; do
            echo "Checking with $gds:" >> $run_tap
            export PMIX_MCA_gds=$gds
            pmix_run_tests
        done
        echo "Checking with auto gds:" >> $run_tap
        export PMIX_MCA_gds=""
        pmix_run_tests


        rm -Rf ${pmix_dir} ${build_dir}
        rc=$((test_ret+rc))

        # Test pmix/dstore/flock
        echo "--------------------------- Building with dstore/flock ----------------------------------------"
        mkdir ${build_dir}
        cd ${build_dir}
        echo ${WORKSPACE}/configure --prefix=$pmix_dir $configure_args --disable-visibility --disable-dstore-pthlck | bash -xeE
        make $make_opt install
        echo "--------------------------- Checking with dstore/flock ----------------------------------------"
        echo "----dstore/flock----" >> $run_tap

        gds_list="hash ds12,hash"
        # check the existence of ds21 component
        # if [ "$has_gds_ds21" -gt 0 ]; then
        #     gds_list="$gds_list ds21,hash"
        # fi

        for gds in $gds_list; do
            echo "Checking with $gds:" >> $run_tap
            export PMIX_MCA_gds=$gds
            pmix_run_tests
        done
        echo "Checking with auto gds:" >> $run_tap
        export PMIX_MCA_gds=""
        pmix_run_tests

        # rm -Rf ${pmix_dir} ${build_dir}
        rc=$((test_ret+rc))
    fi

    unset TMPDIR
    # rmdir $OUTDIR
    cat $WORKSPACE/run_test.tap
    exit $rc

fi
