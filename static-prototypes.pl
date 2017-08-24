use English;

if (/static/) {
	s/inline //;
	s/static //;
	s/__attribute__ \(\(used\)\) //;
	s/^\s+/ /; s/,\n/,/;
	s/\)/\);/;
	s#//.*##;
	print;
}
