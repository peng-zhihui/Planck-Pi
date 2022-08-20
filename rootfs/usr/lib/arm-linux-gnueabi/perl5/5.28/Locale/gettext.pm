package Locale::gettext;

=head1 NAME

Locale::gettext - message handling functions

=head1 SYNOPSIS

    use Locale::gettext;
    use POSIX;     # Needed for setlocale()

    setlocale(LC_MESSAGES, "");

    # OO interface
    my $d = Locale::gettext->domain("my_program");

    print $d->get("Welcome to my program"), "\n";
            # (printed in the local language)

    # Direct access to C functions
    textdomain("my_program");

    print gettext("Welcome to my program"), "\n";
            # (printed in the local language)

=head1 DESCRIPTION

The gettext module permits access from perl to the gettext() family of
functions for retrieving message strings from databases constructed
to internationalize software.

=cut

use Carp;
use POSIX qw(:locale_h);

require Exporter;
require DynaLoader;
@ISA = qw(Exporter DynaLoader);

BEGIN {
	eval {
		require Encode;
		$encode_available = 1;
	};
	import Encode if ($encode_available);
}

$VERSION = "1.07" ;

%EXPORT_TAGS = (

    locale_h =>	[qw(LC_CTYPE LC_NUMERIC LC_TIME LC_COLLATE LC_MONETARY LC_MESSAGES LC_ALL)],

    libintl_h => [qw(gettext textdomain bindtextdomain dcgettext dgettext ngettext dngettext dcngettext bind_textdomain_codeset)],

);

Exporter::export_tags();

@EXPORT_OK = qw(
);

bootstrap Locale::gettext $VERSION;

sub AUTOLOAD {
    local $! = 0;
    my $constname = $AUTOLOAD;
    $constname =~ s/.*:://;
    my $val = constant($constname, (@_ ? $_[0] : 0));
    if ($! == 0) {
	*$AUTOLOAD = sub { $val };
    }
    else {
	croak "Missing constant $constname";
    }
    goto &$AUTOLOAD;
}

=over 2

=item $d = Locale::gettext->domain(DOMAIN)

=item $d = Locale::gettext->domain_raw(DOMAIN)

Creates a new object for retrieving strings in the domain B<DOMAIN>
and returns it. C<domain> requests that strings be returned as
Perl strings (possibly with wide characters) if possible while
C<domain_raw> requests that octet strings directly from functions
like C<dgettext()>.

=cut

sub domain_raw {
	my ($class, $domain) = @_;
	my $self = { domain => $domain, raw => 1 };
	bless $self, $class;
}

sub domain {
	my ($class, $domain) = @_;
	unless ($encode_available) {
		croak "Encode module not available, cannot use Locale::gettext->domain";
	}
	my $self = { domain => $domain, raw => 0 };
	bless $self, $class;
	eval { bind_textdomain_codeset($self->{domain}, "UTF-8"); };
	if ($@ =~ /not implemented/) {
		# emulate it
		$self->{emulate} = 1;
	} elsif ($@ ne '') {
		die;	# some other problem
	}
	$self;
}

=item $d->get(MSGID)

Calls C<dgettext()> to return the translated string for the given
B<MSGID>.

=cut

sub get {
	my ($self, $msgid) = @_;
	$self->_convert(dgettext($self->{domain}, $msgid));
}

=item $d->cget(MSGID, CATEGORY)

Calls C<dcgettext()> to return the translated string for the given
B<MSGID> in the given B<CATEGORY>.

=cut

sub cget {
	my ($self, $msgid, $category) = @_;
	$self->_convert(dcgettext($self->{domain}, $msgid, $category));
}

=item $d->nget(MSGID, MSGID_PLURAL, N)

Calls C<dngettext()> to return the translated string for the given
B<MSGID> or B<MSGID_PLURAL> depending on B<N>.

=cut

sub nget {
	my ($self, $msgid, $msgid_plural, $n) = @_;
	$self->_convert(dngettext($self->{domain}, $msgid, $msgid_plural, $n));
}

=item $d->ncget(MSGID, MSGID_PLURAL, N, CATEGORY)

Calls C<dngettext()> to return the translated string for the given
B<MSGID> or B<MSGID_PLURAL> depending on B<N> in the given
B<CATEGORY>.

=cut

sub ncget {
	my ($self, $msgid, $msgid_plural, $n, $category) = @_;
	$self->_convert(dcngettext($self->{domain}, $msgid, $msgid_plural, $n, $category));
}

=item $d->dir([NEWDIR])

If B<NEWDIR> is given, calls C<bindtextdomain> to set the
name of the directory where messages for the domain
represented by C<$d> are found. Returns the (possibly changed)
current directory name.

=cut

sub dir {
	my ($self, $newdir) = @_;
	if (defined($newdir)) {
		bindtextdomain($self->{domain}, $newdir);
	} else {
		bindtextdomain($self->{domain});
	}
}

=item $d->codeset([NEWCODE])

For instances created with C<Locale::gettext-E<gt>domain_raw>, manuiplates
the character set of the returned strings.
If B<NEWCODE> is given, calls C<bind_textdomain_codeset> to set the
character encoding in which messages for the domain
represented by C<$d> are returned. Returns the (possibly changed)
current encoding name.

=cut

sub codeset {
	my ($self, $codeset) = @_;
	if ($self->{raw} < 1) {
		warn "Locale::gettext->codeset: meaningful only for instances created with domain_raw";
		return;
	}
	if (defined($codeset)) {
		bind_textdomain_codeset($self->{domain}, $codeset);
	} else {
		bind_textdomain_codeset($self->{domain});
	}
}

sub _convert {
	my ($self, $str) = @_;
	return $str if ($self->{raw});
	# thanks to the use of UTF-8 in bind_textdomain_codeset, the
	# result should always be valid UTF-8 when raw mode is not used.
	if ($self->{emulate}) {
		delete $self->{emulate};
		$self->{raw} = 1;
		my $null = $self->get("");
		if ($null =~ /charset=(\S+)/) {
			$self->{decode_from} = $1;
			$self->{raw} = 0;
		} #else matches the behaviour of glibc - no null entry
		  # means no conversion is done
	}
	if ($self->{decode_from}) {
		return decode($self->{decode_from}, $str);
	} else {
		return decode_utf8($str);
	}
}

sub DESTROY {
	my ($self) = @_;
}

=back

gettext(), dgettext(), and dcgettext() attempt to retrieve a string
matching their C<msgid> parameter within the context of the current
locale. dcgettext() takes the message's category and the text domain
as parameters while dgettext() defaults to the LC_MESSAGES category
and gettext() defaults to LC_MESSAGES and uses the current text domain.
If the string is not found in the database, then C<msgid> is returned.

ngettext(), dngettext(), and dcngettext() function similarily but
implement differentiation of messages between singular and plural.
See the documentation for the corresponding C functions for details.

textdomain() sets the current text domain and returns the previously
active domain.

I<bindtextdomain(domain, dirname)> instructs the retrieval functions to look
for the databases belonging to domain C<domain> in the directory
C<dirname>

I<bind_textdomain_codeset(domain, codeset)> instructs the retrieval
functions to translate the returned messages to the character encoding
given by B<codeset> if the encoding of the message catalog is known.

=head1 NOTES

Not all platforms provide all of the functions. Functions that are
not available in the underlying C library will not be available in
Perl either.

Perl programs should use the object interface. In addition to being
able to return native Perl wide character strings,
C<bind_textdomain_codeset> will be emulated if the C library does
not provide it.

=head1 VERSION

1.07.

=head1 SEE ALSO

gettext(3i), gettext(1), msgfmt(1)

=head1 AUTHOR

Kim Vandry <vandry@TZoNE.ORG>

=cut

1;
