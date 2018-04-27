#!/usr/bin/env perl

use English;
use strict;
use warnings;

my $compilerText = `$ENV{CC} --version`;
my @compilerLines = split /\n/, $compilerText;
chomp @compilerLines;

my $git = `which git`;
my $gitHash = '';
if ($git && -d '.git') {
	chomp $git;
	my $hash = `$git rev-parse HEAD`;
	chomp $hash;

	my $dirty = '';
	my @changed = `git diff-index --name-only HEAD`;
	if (@changed) {
		$dirty = '-dirty';
	}

    $gitHash = "\"Git hash: $hash$dirty\\n\"";
}

my $platform = `uname -a`;
chomp $platform;

print <<"EOF";
const char *libocxl_info =
    "LibOCXL Version: $ENV{VERSION_MAJOR}.$ENV{VERSION_MINOR}\\n"
    "CC: $ENV{CC}\\n"
    "Compiler Version: $compilerLines[0]\\n"
    "CFLAGS: $ENV{CFLAGS}\\n"
    $gitHash
    "Platform: $platform\\n\\n";
EOF
