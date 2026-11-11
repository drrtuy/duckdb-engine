# Wait (up to 30 s) until the server error log has a line containing
# $quack_log_pattern (a literal string); fail otherwise. MDEV-41383 tests.
let QUACK_WAIT_PATTERN= $quack_log_pattern;
--perl
my $file= "$ENV{MYSQLTEST_VARDIR}/log/mysqld.1.err";
my $pattern= $ENV{QUACK_WAIT_PATTERN};
die "quack_log_pattern is not set\n" unless length($pattern);
for (1 .. 300)
{
  if (open(my $fh, '<', $file))
  {
    my @lines= <$fh>;
    close($fh);
    if (grep { index($_, $pattern) >= 0 } @lines)
    {
      print "Found in the error log: $pattern\n";
      exit 0;
    }
  }
  select(undef, undef, undef, 0.1);
}
die "Not found in the error log: $pattern\n";
EOF
