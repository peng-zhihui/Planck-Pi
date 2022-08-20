#!/usr/bin/mawk -f

#  eatc.awk
#  another program to remove comments
#


{  while( t = index($0 , "/*") )
   {
     printf "%s" , substr($0,1,t-1)
     $0 = eat_comment( substr($0, t+2) )
   }

   print 
}


function eat_comment(s,		t)
{
  #replace comment by one space
  printf " "

  while ( (t = index(s, "*/")) == 0 )
	if ( getline s == 0 )
	{ # input error -- unterminated comment
          system("/bin/sh -c 'echo unterminated comment' 1>&2")
	  exit 1
	}

  return  substr(s,t+2)
}

