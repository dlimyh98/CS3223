VERSION=16.1

ASSIGN_DIR=$HOME/cs3223-assign1
DOWNLOAD_DIR=$HOME
SRC_DIR=${DOWNLOAD_DIR}/postgresql-${VERSION}

echo "Adding our additional testcases..."

# Re-install test_bufmgr extension (with our additional testcases)
cp -r ${ASSIGN_DIR}/test_bufmgr ${SRC_DIR}/contrib
cd ${SRC_DIR}/contrib/test_bufmgr
make && make install 