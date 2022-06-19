#!/bin/sh

test_description='git apply with core.ignorecase'

. ./test-lib.sh

test_expect_success setup '
       # initial commit has file0 only
       test_commit "initial" file0 "initial commit with file0" initial &&

       # current commit has file1 as well
       test_commit "current" file1 "initial content of file1" current &&
       file0blob=$(git rev-parse :file0) &&
       file1blob=$(git rev-parse :file1) &&

       # prepare sample patches
       # file0 is modified
       echo modification to file0 >file0 &&
       git add file0 &&
       modifiedfile0blob=$(git rev-parse :file0) &&

       # file1 is removed and then ...
       git rm --cached file1 &&
       # ... identical copies are placed at File1 and file2
       git update-index --add --cacheinfo 100644,$file1blob,file2 &&
       git update-index --add --cacheinfo 100644,$file1blob,File1 &&

       # then various patches to do basic things
       git diff HEAD^ HEAD -- file1 >creation-patch &&
       git diff HEAD HEAD^ -- file1 >deletion-patch &&
       git diff --cached HEAD -- file1 file2 >rename-file1-to-file2-patch &&
       git diff --cached HEAD -- file1 File1 >rename-file1-to-File1-patch &&
       git diff --cached HEAD -- file0 >modify-file0-patch
'

# Basic creation, deletion, modification and renaming.
test_expect_success 'creation and deletion' '
       # start at "initial" with file0 only
       git reset --hard initial &&

       # add file1
       git -c core.ignorecase=false apply --cached creation-patch &&
       test_cmp_rev :file1 "$file1blob" &&

       # remove file1
       git -c core.ignorecase=false apply --cached deletion-patch &&
       test_must_fail git rev-parse --verify :file1 &&

       # do the same with ignorecase
       git -c core.ignorecase=true apply --cached creation-patch &&
       test_cmp_rev :file1 "$file1blob" &&
       git -c core.ignorecase=true apply --cached deletion-patch &&
       test_must_fail git rev-parse --verify :file1
'

test_expect_success 'modificaiton' '
       # start at "initial" with file0 only
       git reset --hard initial &&

       # modify file0
       git -c core.ignorecase=false apply --cached modify-file0-patch &&
       test_cmp_rev :file0 "$modifiedfile0blob" &&
       git -c core.ignorecase=false apply --cached -R modify-file0-patch &&
       test_cmp_rev :file0 "$file0blob" &&

       # do the same with ignorecase
       git -c core.ignorecase=true apply --cached modify-file0-patch &&
       test_cmp_rev :file0 "$modifiedfile0blob" &&
       git -c core.ignorecase=true apply --cached -R modify-file0-patch &&
       test_cmp_rev :file0 "$file0blob"
'

test_expect_success 'rename file1 to file2' '
       # start from file0 and file1
       git reset --hard current &&

       # rename file1 to file2
       git -c core.ignorecase=false apply --cached rename-file1-to-file2-patch &&
       test_must_fail git rev-parse --verify :file1 &&
       test_cmp_rev :file2 "$file1blob" &&
       git -c core.ignorecase=false apply --cached -R rename-file1-to-file2-patch &&
       test_must_fail git rev-parse --verify :file2 &&
       test_cmp_rev :file1 "$file1blob" &&

       # do the same with ignorecase
       git -c core.ignorecase=true apply --cached rename-file1-to-file2-patch &&
       test_must_fail git rev-parse --verify :file1 &&
       test_cmp_rev :file2 "$file1blob" &&
       git -c core.ignorecase=true apply --cached -R rename-file1-to-file2-patch &&
       test_must_fail git rev-parse --verify :file2 &&
       test_cmp_rev :file1 "$file1blob"
'

test_expect_success 'rename file1 to file2' '
       # start from file0 and file1
       git reset --hard current &&

       # rename file1 to File1
       git -c core.ignorecase=false apply --cached rename-file1-to-File1-patch &&
       test_must_fail git rev-parse --verify :file1 &&
       test_cmp_rev :File1 "$file1blob" &&
       git -c core.ignorecase=false apply --cached -R rename-file1-to-File1-patch &&
       test_must_fail git rev-parse --verify :File1 &&
       test_cmp_rev :file1 "$file1blob" &&

       # do the same with ignorecase
       git -c core.ignorecase=true apply --cached rename-file1-to-File1-patch &&
       test_must_fail git rev-parse --verify :file1 &&
       test_cmp_rev :File1 "$file1blob" &&
       git -c core.ignorecase=true apply --cached -R rename-file1-to-File1-patch &&
       test_must_fail git rev-parse --verify :File1 &&
       test_cmp_rev :file1 "$file1blob"
'

# We may want to add tests with working tree here, without "--cached" and
# with and without "--index" here.  For example, should modify-file0-patch
# apply cleanly if we have File0 with $file0blob in the index and the working
# tree if core.icase is set?

test_expect_success CASE_INSENSITIVE_FS 'a test only for icase fs' '
       : sample
'

test_expect_success !CASE_INSENSITIVE_FS 'a test only for !icase fs' '
       : sample
'

test_done
