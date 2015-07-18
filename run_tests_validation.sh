#! /bin/bash



cd run_tests_validation


# only execute scripts which do not start with an underscore
for i in [^_]*.sh; do
	echo "******************************************************"
	echo "* Executing script $i"
	echo "******************************************************"
	./$i
done
