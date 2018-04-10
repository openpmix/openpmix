import numpy as np
import pandas as pd
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

f = open("memory_info.csv","r")
lines = f.readlines()
f.close()
f = open("selected.csv","w")

#insert table title
f.write("Rank,VmRSS(status),VmSize,Rss(smaps),Pss,Ppid,pVmRSS(status),pVmSize,pRss(smaps),pPss,mapwr,pmapwr,meminfo\n")

#Select lines with key value MEM_info
for line in lines:
    if "MEM_info" in line:
        f.write(line)
f.close()

data = pd.read_csv('selected.csv', sep=',')
print data

#mean/std of mpi process pss, mapwr
pss_mean = np.mean(data["Pss"])
pss_std = np.std(data['Pss'])

mapwr_mean = np.mean(data["mapwr"])
mapwr_std = np.std(data['mapwr'])

#mean/std of parent pss, mapwr
pPss_mean = np.mean(data["pPss"])
pPss_std = np.std(data['pPss'])

pmapwr_mean = np.mean(data["pmapwr"])
pmapwr_std = np.std(data['pmapwr'])

print "Mpi process pss mean: ", pss_mean, "std: ", pss_std, "max: ", np.max(data['Pss']), "min: ", np.min(data['Pss'])
print "Mpi process mapwr mean: ", mapwr_mean, "std", mapwr_std, "max: ", np.max(data['mapwr']), "min: ", np.min(data['mapwr'])

print "Daemon pss mean: ", pPss_mean, "std: ", pPss_std, "max: ", np.max(data['pPss']), "min: ", np.min(data['pPss'])
print "Daemon mapwr mean: ", pmapwr_mean, "std: ", pmapwr_std, "max: ", np.max(data['pmapwr']), "min: ", np.min(data['pmapwr'])

#find outliers with range 0.1*mean
for index, row in data.iterrows() :
    if abs(row['Pss'] - pss_mean) > (pss_mean * 0.1):
        print "Outlier Mpi process Rank/Pss", row['Rank'],row['Pss']
    if abs(row['pPss'] - pPss_mean) > (pPss_mean * 0.1):
        print "Outlier Daemon Rank/Pss", row['Rank'],row['pPss']
    if abs(row['mapwr'] - mapwr_mean) > (mapwr_mean * 0.1):
        print "Outlier Mpi process Rank/mapwr", row['Rank'],row['mapwr']
    if abs(row['pmapwr'] - pmapwr_mean) > (pmapwr_mean * 0.1):
        print "Outlier Daemon Rank/mapwr", row['Rank'],row['pmapwr']

plt.figure()
plt.ylabel('Mpi process memory usage Pss(KB)', fontsize=14, color='blue')
plt.xlabel('Mpi Rank', fontsize=14, color='blue')
#red dashes, blue squares
#plt.plot(data["Pss"], 'r--', data["pPss"], 'bs')
plt.plot(data["Rank"], data["Pss"], 'rs')
plt.savefig('Mpi_process_pss.pdf')

plt.figure()
data.boxplot(column=["Pss"])
plt.savefig('Mpi_process_pss_boxplot.pdf')

plt.figure()
plt.ylabel('Daemon memory usage Pss(KB)', fontsize=14, color='blue')
plt.xlabel('Child Mpi Rank', fontsize=14, color='blue')
#red dashes, blue squares
plt.plot(data["Rank"],data["pPss"], 'rs')
plt.savefig('Daemon_pss.pdf')

plt.figure()
data.boxplot(column=["pPss"])
plt.savefig('Daemon_pss_boxplot.pdf')

plt.figure()
plt.ylabel('Mpi process w/r memory usage(KB)', fontsize=14, color='blue')
plt.xlabel('Mpi Rank', fontsize=14, color='blue')
plt.plot(data["Rank"], data["mapwr"], 'rs')
plt.savefig('Mpi_process_mapwr.pdf')

plt.figure()
data.boxplot(column=["mapwr"])
plt.savefig('Mpi_process_mapwr_boxplot.pdf')

plt.figure()
plt.ylabel('Daemon memory w/r usage(KB)', fontsize=14, color='blue')
plt.xlabel('Child Mpi Rank', fontsize=14, color='blue')
#red dashes, blue squares
plt.plot(data["Rank"],data["pmapwr"], 'rs')
plt.savefig('Daemon_pmapwr.pdf')

plt.figure()
data.boxplot(column=["pmapwr"])
plt.savefig('Daemon_pmapwr_boxplot.pdf')
