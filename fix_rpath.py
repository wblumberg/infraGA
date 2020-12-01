import glob
import os

bins = glob.glob('bin/*')
for b in bins:
    cmd = "install_name_tool -change \"@rpath/libfftw3.3.dylib\" \"/Users/wgblumbe/miniconda3/envs/py37/lib/libfftw3.3.dylib\" " + b
    print(cmd)
    os.system(cmd)
    os.system('otool -L ' + b)
 
