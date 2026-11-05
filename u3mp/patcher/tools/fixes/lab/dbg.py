import sys,traceback
sys.path[:0]=[sys.argv[1],sys.argv[1]+'/stage']
import stagepipe
try:
  stagepipe.run('lab-stage','../labsrc/pak',"{{PS4}}/u3data/build/main/pak23/ps4",'res','work','out')
except BaseException:traceback.print_exc()
