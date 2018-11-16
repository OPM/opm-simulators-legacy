#!/bin/bash

#Print commands as they execute
#set -x

declare -a upstreams
upstreams=(libecl
           opm-common
           opm-material
           opm-grid
           ewoms
           opm-simulators)

declare -A upstreamRev
upstreamRev[libecl]=master
upstreamRev[opm-common]=master
upstreamRev[opm-material]=master
upstreamRev[opm-grid]=master
upstreamRev[ewoms]=master
upstreamRev[opm-simulators]=master

if grep -q "opm-common=" <<< $ghprbCommentBody
then
  upstreamRev[opm-common]=pull/`echo $ghprbCommentBody | sed -r 's/.*opm-common=([0-9]+).*/\1/g'`/merge
fi

# No downstreams currently
declare -a downstreams
declare -A downstreamRev

# Clone opm-common
pushd .
mkdir -p $WORKSPACE/deps/opm-common
cd $WORKSPACE/deps/opm-common
git init .
git remote add origin https://github.com/OPM/opm-common
git fetch --depth 1 origin ${upstreamRev[opm-common]}:branch_to_build
test $? -eq 0 || exit 1
git checkout branch_to_build
popd

source $WORKSPACE/deps/opm-common/jenkins/build-opm-module.sh

parseRevisions
printHeader opm-simulators-legacy

# Setup opm-data
source $WORKSPACE/deps/opm-common/jenkins/setup-opm-tests.sh

build_module_full opm-simulators-legacy
