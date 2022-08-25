#!/usr/bin/mawk -f

#  ct_length.awk
#
#  replaces all length 
#  by  length($0)
#


{

  while ( i = index($0, "length") )
  {
     printf "%s" , substr($0,1, i+5)  # ...length
     $0 = substr($0,i+6)

     if ( match($0, /^[ \t]*\(/) )
     {
       # its OK
       printf "%s", substr($0, 1, RLENGTH)
       $0 = substr($0, RLENGTH+1)
     }
     else # length alone
       printf "($0)"

  }
  print
}
