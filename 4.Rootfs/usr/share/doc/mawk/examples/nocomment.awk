#!/usr/bin/mawk -f

# remove C comments  from a list of files
# using a comment as the record separator
#
# this is trickier than I first thought
# The first version in .97-.9993 was wrong

BEGIN {
 # RS is set to  a comment (this is mildly tricky, I blew it here
 RS = "/\*([^*]|\*+[^*/])*\*+/"
 ORS = " "
 getline hold
 filename = FILENAME
}

# if changing files
filename != FILENAME {
  filename = FILENAME
  printf "%s" , hold
  hold = $0
  next
}

{ # hold one record because we don't want ORS on the last
  #  record in each file
  print hold
  hold = $0
}

END { printf "%s", hold }
