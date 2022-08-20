package utf8;

$utf8::hint_bits = 0x00800000;

our $VERSION = '1.21';

sub import {
    $^H |= $utf8::hint_bits;
}

sub unimport {
    $^H &= ~$utf8::hint_bits;
}

sub AUTOLOAD {
    require "utf8_heavy.pl";
    goto &$AUTOLOAD if defined &$AUTOLOAD;
    require Carp;
    Carp::croak("Undefined subroutine $AUTOLOAD called");
}

1;
__END__

