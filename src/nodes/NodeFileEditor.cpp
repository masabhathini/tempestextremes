///////////////////////////////////////////////////////////////////////////////
///
///	\file    NodeFileEditor.cpp
///	\author  Paul Ullrich
///	\version August 30, 2018
///
///	<remarks>
///		Copyright 2000-2018 Paul Ullrich
///
///		This file is distributed as part of the Tempest source code package.
///		Permission is granted to use, copy, modify and distribute this
///		source code and its documentation under the terms of the GNU General
///		Public License.  This software is provided "as is" without express
///		or implied warranty.
///	</remarks>

#include "CommandLine.h"
#include "Exception.h"
#include "Announce.h"
#include "Variable.h"
#include "AutoCurator.h"
#include "DataMatrix.h"
#include "ArgumentTree.h"
#include "STLStringHelper.h"
#include "NodeFileUtilities.h"

#include "netcdfcpp.h"

#include <fstream>
#include <queue>
#include <set>

#if defined(TEMPEST_MPIOMP)
#include <mpi.h>
#endif

///////////////////////////////////////////////////////////////////////////////

void CalculateRadialProfile(
	VariableRegistry & varreg,
	NcFileVector & vecFiles,
	const SimpleGrid & grid,
	const ColumnDataHeader & cdh,
	int iTime,
	PathNode & pathnode,
	VariableIndex varixU,
	VariableIndex varixV,
	std::string strBins,
	std::string strBinWidth
) {
	// Get number of bins
	int nBins = pathnode.GetColumnDataAsInteger(cdh, strBins);

	// Get bin width
	double dBinWidth = pathnode.GetColumnDataAsDouble(cdh, strBinWidth);

	// Check arguments
	if (nBins <= 0) {
		_EXCEPTIONT("\nNonpositive value of <bins> argument given");
	}
	if (dBinWidth <= 0.0) {
		_EXCEPTIONT("\nNonpositive value of <bin_width> argument given");
	}
	if (static_cast<double>(nBins) * dBinWidth > 180.0) {
		_EXCEPTIONT("\n<bins> x <bin_width> must be no larger than 180 (degrees)");
	}

	// Get the center grid index
	const int ix0 = pathnode.m_gridix;

	// Load the zonal wind data
	Variable & varU = varreg.Get(varixU);
	varU.LoadGridData(varreg, vecFiles, grid, iTime);
	const DataVector<float> & dataStateU = varU.GetData();

	// Load the meridional wind data
	Variable & varV = varreg.Get(varixV);
	varV.LoadGridData(varreg, vecFiles, grid, iTime);
	const DataVector<float> & dataStateV = varV.GetData();

	// Verify that dRadius is less than 180.0
	double dRadius = dBinWidth * static_cast<double>(nBins);

	if ((dRadius < 0.0) || (dRadius > 180.0)) {
		_EXCEPTIONT("Radius must be in the range [0.0, 180.0]");
	}

	// Check grid index
	if (ix0 >= grid.m_vecConnectivity.size()) {
		_EXCEPTION2("Grid index (%i) out of range (< %i)",
			ix0, static_cast<int>(grid.m_vecConnectivity.size()));
	}

	// Central lat/lon and Cartesian coord
	double dLon0 = grid.m_dLon[ix0];
	double dLat0 = grid.m_dLat[ix0];

	double dX0 = cos(dLon0) * cos(dLat0);
	double dY0 = sin(dLon0) * cos(dLat0);
	double dZ0 = sin(dLat0);

	// Allocate bins
	std::vector< std::vector<double> > dVelocities;
	dVelocities.resize(nBins);

	// Queue of nodes that remain to be visited
	std::queue<int> queueNodes;
	for (int n = 0; n < grid.m_vecConnectivity[ix0].size(); n++) {
		queueNodes.push(grid.m_vecConnectivity[ix0][n]);
	}

	// Set of nodes that have already been visited
	std::set<int> setNodesVisited;

	// Loop through all latlon elements
	while (queueNodes.size() != 0) {
		int ix = queueNodes.front();
		queueNodes.pop();

		if (setNodesVisited.find(ix) != setNodesVisited.end()) {
			continue;
		}

		setNodesVisited.insert(ix);

		// Don't perform calculation on central node
		if (ix == ix0) {
			continue;
		}

		// lat/lon and Cartesian coords of this point
		double dLat = grid.m_dLat[ix];
		double dLon = grid.m_dLon[ix];

		double dX = cos(dLon) * cos(dLat);
		double dY = sin(dLon) * cos(dLat);
		double dZ = sin(dLat);

		// Great circle distance to this element (in degrees)
		double dR =
			sin(dLat0) * sin(dLat)
			+ cos(dLat0) * cos(dLat) * cos(dLon - dLon0);

		if (dR >= 1.0) {
			dR = 0.0;
		} else if (dR <= -1.0) {
			dR = 180.0;
		} else {
			dR = 180.0 / M_PI * acos(dR);
		}
		if (dR != dR) {
			_EXCEPTIONT("NaN value detected");
		}

		if (dR >= dRadius) {
			continue;
		}

		// Velocities at this location
		double dUlon = dataStateU[ix];
		double dUlat = dataStateV[ix];

		// Cartesian velocities at this location
		double dUx = - sin(dLat) * cos(dLon) * dUlat - sin(dLon) * dUlon;
		double dUy = - sin(dLat) * sin(dLon) * dUlat + cos(dLon) * dUlon;
		double dUz = cos(dLat) * dUlat;

		double dUtot = sqrt(dUlon * dUlon + dUlat * dUlat);

		// Calculate local radial vector from central lat/lon
		// i.e. project \vec{X} - \vec{X}_0 to the surface of the
		//      sphere and normalize to unit length.
		double dRx = dX - dX0;
		double dRy = dY - dY0;
		double dRz = dZ - dZ0;

		double dDot = dRx * dX + dRy * dY + dRz * dZ;

		dRx -= dDot * dX;
		dRy -= dDot * dY;
		dRz -= dDot * dZ;

		double dMag = sqrt(dRx * dRx + dRy * dRy + dRz * dRz);

		dRx /= dMag;
		dRy /= dMag;
		dRz /= dMag;

		// Calculate local azimuthal velocity vector
		double dAx = dY * dRz - dZ * dRy;
		double dAy = dZ * dRx - dX * dRz;
		double dAz = dX * dRy - dY * dRx;
/*
		dDot = dAx * dAx + dAy * dAy + dAz * dAz;
		if (fabs(dDot - 1.0) > 1.0e-10) {
			std:: cout << dDot << std::endl;
			_EXCEPTIONT("Logic error");
		}
*/
		// Calculate radial velocity
		//double dUr = dUx * dRx + dUy * dRy + dUz * dRz;

		// Calculate azimuthal velocity
		double dUa = dUx * dAx + dUy * dAy + dUz * dAz;

		//printf("%1.5e %1.5e :: %1.5e %1.5e\n", dUlon, dUlat, dUr, dUa);

		// Determine bin
		int iBin = static_cast<int>(dR / dBinWidth);
		if (iBin >= nBins) {
			_EXCEPTIONT("Logic error");
		}

		dVelocities[iBin].push_back(dUa);

		if (iBin < nBins-1) {
			dVelocities[iBin+1].push_back(dUa);
		}

		// Add all neighbors of this point
		for (int n = 0; n < grid.m_vecConnectivity[ix].size(); n++) {
			queueNodes.push(grid.m_vecConnectivity[ix][n]);
		}
	}

	// Construct radial profile
	ColumnDataRadialVelocityProfile * pdat =
		new ColumnDataRadialVelocityProfile;

	pathnode.PushColumnData(pdat);

	pdat->m_dR.resize(nBins);
	pdat->m_dUa.resize(nBins);
	pdat->m_dUr.resize(nBins);

	pdat->m_dR[0] = 0.0;
	pdat->m_dUa[0] = 0.0;
	pdat->m_dUr[0] = 0.0;
	for (int i = 1; i < nBins; i++) {
		double dAvg = 0.0;
		if (dVelocities[i].size() != 0) {
			for (int j = 0; j < dVelocities[i].size(); j++) {
				dAvg += dVelocities[i][j];
			}
			dAvg /= static_cast<double>(dVelocities[i].size());
		}
		pdat->m_dR[i] = static_cast<double>(i) * dBinWidth;
		pdat->m_dUa[i] = dAvg;
		pdat->m_dUr[i] = 0.0;
	}
}

///////////////////////////////////////////////////////////////////////////////

void SumRadius(
	VariableRegistry & varreg,
	NcFileVector & vecFiles,
	const SimpleGrid & grid,
	const ColumnDataHeader & cdh,
	int iTime,
	PathNode & pathnode,
	VariableIndex varix,
	std::string strRadius
) {
	// Get the radius 
	double dRadius = pathnode.GetColumnDataAsDouble(cdh, strRadius);

	if (dRadius == 0.0) {
		pathnode.PushColumnData(
			new ColumnDataDouble(0.0));
	} else {
		pathnode.PushColumnData(
			new ColumnDataDouble(1.0));
	}
}

///////////////////////////////////////////////////////////////////////////////

///	<summary>
///		Polynomial cubic Hermite interpolation function using Fritsch-Carlson
///		method.
///	</summary>
///	<reference>
///		https://en.wikipedia.org/wiki/Monotone_cubic_interpolation
///	</reference>
///	<param name="n">
///		Number of points to use in interpolant.
///	</param>
///	<param name="x">
///		Array of size n with independent coordinate values of interpolant.
///		The values of x must be monotone increasing or behavior is undefined.
///	</param>
///	<param name="y">
///		Array of size n with dependent coordinate values of interpolant.
///	</param>
///	<param name="c">
///		Output array of cubic polynomial coefficients of size (n-1) x 4.
///		The interpolation function is then defined by
///		  xdiff = x - x[i]
///		  f_i(x) = c[i][0]
///		         + xdiff * (c[i][1] + xdiff * (c[i][2] + xdiff * c[i][3]))
///	</param>
///	<param name="work">
///		Work array of size 3*n.
///	</param>
///	<returns>
///		(-1) if the value of n is less than 2.
///		(0) if the calculation completed successfully.
///	</returns>
int pchip_fc_coeff(
	int n,
	double * const x,
	double * const y,
	double * c,
	double * work
) {
	if (n < 2) {
		return (-1);
	}

	double * dx = work;
	double * dy = work+n;
	double * m = work+2*n;

	for (int i = 0; i < n-1; i++) {
		dx[i] = x[i+1] - x[i];
		dy[i] = y[i+1] - y[i];
		m[i] = dy[i] / dx[i];
		c[i*4] = y[i];
	}
	m[n-1] = dy[n-2] / dx[n-2];

	c[1] = m[0];
	for (int i = 0; i < n-2; i++) {
		double mi = m[i];
		double mn = m[i+1];
		if (mi * mn <= 0.0) {
			c[4*(i+1)+1] = 0.0;
		} else {
			double dc = dx[i] + dx[i+1];
			c[4*(i+1)+1] = 3.0 * dc / ((dc + dx[i+1]) / mi + (dc + dx[i]) / mn);
		}
	}

	for (int i = 0; i < n-2; i++) {
		double c1 = c[4*i+1];
		double invdx = 1.0/dx[i];
		double dc = c1 + c[4*(i+1)+1] - 2.0 * m[i];

		c[4*i+2] = (m[i] - c1 - dc) * invdx;
		c[4*i+3] = dc * invdx * invdx;
	}
	{
		int i = n-2;
		double c1 = c[4*i+1];
		double invdx = 1.0/dx[i];
		double dc = c1 + m[n-1] - 2.0 * m[i];

		c[4*i+2] = (m[i] - c1 - dc) * invdx;
		c[4*i+3] = dc * invdx * invdx;
	}

	return 0;
}

///////////////////////////////////////////////////////////////////////////////

void CalculateStormVelocity(
	const SimpleGrid & grid,
	Path & path
) {
	const double MetersPerRadian = 111.325 * 1000.0 * 180.0 / M_PI;

	int nPathNodes = path.m_vecPathNodes.size();
	if (path.m_vecPathNodes.size() < 2) {
		_EXCEPTIONT("Path must contain at least two nodes");
	}

	DataVector<double> dX(nPathNodes);
	DataVector<double> dY(nPathNodes);
	DataMatrix<double> dC(nPathNodes-1, 4);
	DataVector<double> dWork(nPathNodes*3);

	// Velocity column data
	std::vector<ColumnDataRLLVelocity *> vecPathNodeVelocity;
	vecPathNodeVelocity.resize(nPathNodes);

	// Calculate meridional velocity
	for (int i = 0; i < nPathNodes; i++) {
		PathNode & pathnode = path.m_vecPathNodes[i];

		dX[i] = pathnode.m_time - path.m_timeStart;

		if (pathnode.m_gridix >= grid.m_dLat.GetRows()) {
			_EXCEPTION2("Grid index of path (%i) out of range "
				"(only %i nodes in grid)",
				pathnode.m_gridix,
				grid.m_dLat.GetRows());
		}

		dY[i] = grid.m_dLat[pathnode.m_gridix];

		ColumnDataRLLVelocity * pcd = new ColumnDataRLLVelocity();
		vecPathNodeVelocity.push_back(pcd);
		pathnode.m_vecColumnData.push_back(pcd);
	}

	if (vecPathNodeVelocity.size() != nPathNodes) {
		_EXCEPTIONT("Logic error");
	}

	const double dFinalDeltaT = dX[nPathNodes-1] - dX[nPathNodes-2];

	pchip_fc_coeff(nPathNodes, &(dX[0]), &(dY[0]), &(dC[0][0]), &(dWork[0]));

	// Array of velocity column data
	for (int i = 0; i < nPathNodes-1; i++) {
		PathNode & pathnode = path.m_vecPathNodes[i];
		vecPathNodeVelocity[i]->m_dV = dC[i][1];
	}
	vecPathNodeVelocity[nPathNodes-1]->m_dV =
		dC[nPathNodes-2][1]
		+ 2.0 * dC[nPathNodes-2][2] * dFinalDeltaT
		+ 3.0 * dC[nPathNodes-2][3] * dFinalDeltaT * dFinalDeltaT;

	for (int i = 0; i < nPathNodes; i++) {
		PathNode & pathnode = path.m_vecPathNodes[i];
		vecPathNodeVelocity[i]->m_dV *= MetersPerRadian;
	}

	// Calculate zonal velocity
	for (int i = 0; i < nPathNodes; i++) {
		PathNode & pathnode = path.m_vecPathNodes[i];
		dY[i] = grid.m_dLon[pathnode.m_gridix];
	}

	pchip_fc_coeff(nPathNodes, &(dX[0]), &(dY[0]), &(dC[0][0]), &(dWork[0]));

	for (int i = 0; i < nPathNodes-1; i++) {
		vecPathNodeVelocity[i]->m_dU = dC[i][1];
	}
	vecPathNodeVelocity[nPathNodes-1]->m_dU =
		dC[nPathNodes-2][1]
		+ 2.0 * dC[nPathNodes-2][2] * dFinalDeltaT
		+ 3.0 * dC[nPathNodes-2][3] * dFinalDeltaT * dFinalDeltaT;

	for (int i = 0; i < nPathNodes; i++) {
		PathNode & pathnode = path.m_vecPathNodes[i];
		vecPathNodeVelocity[i]->m_dU *=
			MetersPerRadian * cos(grid.m_dLat[pathnode.m_gridix]);
	}

/*
	for (int i = 0; i < nPathNodes-1; i++) {
		printf("%1.5e\n", path.m_vecPathNodes[i].m_dVelocityLat);
		printf("%1.5e %1.5e :: %1.5e %1.5e %1.5e %1.5e\n", dX[i], dY[i], dC[i][0], dC[i][1], dC[i][2], dC[i][3]); //path.m_vecPathNodes[i].m_dVelocityLat);
	}
	printf("%1.5e\n", path.m_vecPathNodes[nPathNodes-1].m_dVelocityLat);
	printf("%1.5e %1.5e\n", dX[nPathNodes-1], dY[nPathNodes-1]);
*/
/*
	for (int i = 0; i < nPathNodes; i++) {
		PathNode & pathnode = path.m_vecPathNodes[i];
		printf("%1.5e %1.5e\n", pathnode.m_dVelocityLat, pathnode.m_dVelocityLon);
	}
*/
	//_EXCEPTION();
}

///////////////////////////////////////////////////////////////////////////////

int main(int argc, char** argv) {

#if defined(TEMPEST_MPIOMP)
	// Initialize MPI
	MPI_Init(&argc, &argv);

	// Not yet implemented
	int nMPISize;
	MPI_Comm_size(MPI_COMM_WORLD, &nMPISize);
	if (nMPISize > 1) {
		std::cout << "Sorry!  Parallel processing with MPI is not yet implemented" << std::endl;
		return (-1);
	}
#endif

	// Turn off fatal errors in NetCDF
	NcError error(NcError::silent_nonfatal);

	// Enable output only on rank zero
	AnnounceOnlyOutputOnRankZero();

try {

	// Input text file
	std::string strInputFile;

	// Input list of text files
	std::string strInputFileList;

	// Input file type
	std::string strInputFileType;

	// Input data file
	std::string strInputData;

	// Input list of data files
	std::string strInputDataList;

	// Connectivity file
	std::string strConnectivity;

	// Data is regional
	bool fRegional;

	// Input format (columns of in_file)
	std::string strInputFormat;

	// Output format (columns of out_file)
	std::string strOutputFormat;

	// Output file
	std::string strOutputFile;

	// Calculation commands
	std::string strCalculate;

	// List of variables to append
	std::string strAppend;

	// Append output to input file
	bool fOutputAppend;

	// Parse the command line
	BeginCommandLine()
		CommandLineString(strInputFile, "in_file", "");
		//CommandLineString(strInputFileList, "in_file_list", "");
		CommandLineStringD(strInputFileType, "in_file_type", "SN", "[DCU|SN]");
		CommandLineString(strInputData, "in_data", "");
		CommandLineString(strInputDataList, "in_data_list", "");
		CommandLineString(strConnectivity, "in_connect", "");
		CommandLineBool(fRegional, "regional");

		CommandLineString(strInputFormat, "in_fmt", "");
		CommandLineString(strOutputFormat, "out_fmt", "");

		CommandLineString(strOutputFile, "out_file", "");
		//CommandLineBool(fOutputAppend, "out_append");

		CommandLineString(strCalculate, "calculate", "");
		CommandLineString(strAppend, "append", "");

		ParseCommandLine(argc, argv);
	EndCommandLine(argv)

	AnnounceBanner();

	// Create Variable registry
	VariableRegistry varreg;

	// Create autocurator
	AutoCurator autocurator;

	// Check arguments
	if ((strInputFile.length() == 0) && (strInputFileList.length() == 0)) {
		_EXCEPTIONT("No input file (--in_file) or (--in_file_list)"
			" specified");
	}
	if ((strInputFile.length() != 0) && (strInputFileList.length() != 0)) {
		_EXCEPTIONT("Only one of (--in_file) or (--in_file_list)"
			" may be specified");
	}
	if ((strInputData.length() == 0) && (strInputDataList.length() == 0)) {
		_EXCEPTIONT("No input data file (--in_data) or (--in_data_list)"
			" specified");
	}
	if ((strInputData.length() != 0) && (strInputDataList.length() != 0)) {
		_EXCEPTIONT("Only one of (--in_data) or (--in_data_list)"
			" may be specified");
	}

	// Input file type
	InputFileType iftype;
	if (strInputFileType == "DCU") {
		iftype = InputFileTypeDCU;
	} else if (strInputFileType == "SN") {
		iftype = InputFileTypeSN;
	} else {
		_EXCEPTIONT("Invalid --in_file_type, expected \"SN\" or \"DCU\"");
	}

	// Parse in_fmt string
	ColumnDataHeader cdhInput;
	cdhInput.Parse(strInputFormat);

	// Parse out_fmt string
	ColumnDataHeader cdhOutput;
	cdhOutput.Parse(strOutputFormat);

	// Parse calculations
	ArgumentTree calc(true);
	calc.Parse(strCalculate);

	// Define the SimpleGrid
	SimpleGrid grid;

	// Curate input data
	if (strInputData.length() != 0) {
		AnnounceStartBlock("Autocurating in_data");
		autocurator.IndexFiles(strInputData);

	} else {
		AnnounceStartBlock("Autocurating in_data_list");
		std::ifstream ifInputDataList(strInputDataList.c_str());
		if (!ifInputDataList.is_open()) {
			_EXCEPTION1("Unable to open file \"%s\"",
				strInputDataList.c_str());
		}
		std::string strFileLine;
		while (std::getline(ifInputDataList, strFileLine)) {
			if (strFileLine.length() == 0) {
				continue;
			}
			if (strFileLine[0] == '#') {
				continue;
			}
			autocurator.IndexFiles(strFileLine);
		}
	}
	AnnounceEndBlock("Done");

	// Check for connectivity file
	if (strConnectivity != "") {
		AnnounceStartBlock("Generating grid information from connectivity file");
		grid.FromFile(strConnectivity);
		AnnounceEndBlock("Done");

	// No connectivity file; check for latitude/longitude dimension
	} else {
		AnnounceStartBlock("No connectivity file specified");
		Announce("Attempting to generate latitude-longitude grid from data file");
		const std::vector<std::string> & vecFiles = autocurator.GetFilenames();

		if (vecFiles.size() < 1) {
			_EXCEPTIONT("No data files specified; unable to generate grid");
		}

		NcFile ncFile(vecFiles[0].c_str());
		if (!ncFile.is_valid()) {
			_EXCEPTION1("Unable to open NetCDF file \"%s\"", vecFiles[0].c_str());
		}

		grid.GenerateLatitudeLongitude(&ncFile, fRegional);

		if (grid.m_nGridDim.size() != 2) {
			_EXCEPTIONT("Logic error when generating connectivity");
		}
		AnnounceEndBlock("Done");
	}

	// Load input file list
	std::vector<std::string> vecInputFiles;

	if (strInputFile.length() != 0) {
		vecInputFiles.push_back(strInputFile);

	} else {
		std::ifstream ifInputFileList(strInputFileList.c_str());
		if (!ifInputFileList.is_open()) {
			_EXCEPTION1("Unable to open file \"%s\"",
				strInputFileList.c_str());
		}
		std::string strFileLine;
		while (std::getline(ifInputFileList, strFileLine)) {
			if (strFileLine.length() == 0) {
				continue;
			}
			if (strFileLine[0] == '#') {
				continue;
			}
			vecInputFiles.push_back(strFileLine);
		}
	}

	// A map from Times to file lines, used for StitchNodes formatted output
	TimeToPathNodeMap mapTimeToPathNode;

	// Loop over all files
	for (int f = 0; f < vecInputFiles.size(); f++) {

		AnnounceStartBlock("Processing input (%s)", vecInputFiles[f].c_str());

		// Vector of Path information loaded from StitchNodes file
		PathVector pathvec;

		// Read contents of NodeFile into PathVector
		AnnounceStartBlock("Reading file");
		ParseNodeFile(
			vecInputFiles[f],
			iftype,
			cdhInput,
			grid,
			autocurator.GetCalendarType(),
			pathvec,
			mapTimeToPathNode);
		AnnounceEndBlock("Done");

/*
		// Calculate velocity at each point
		if (iftype == InputFileTypeSN) {

			// Calculate trajectory velocity
			Path & path = vecPaths[vecPaths.size()-1];
			CalculateStormVelocity(grid, path);

			// Append velocity to file
			if (fAppendTrajectoryVelocity) {
				for (int i = 0; i < path.m_vecPathNodes.size(); i++) {
					PathNode & pathnode = path.m_vecPathNodes[i];
					if (pathnode.m_vecData.size() < 4) {
						_EXCEPTIONT("Logic error");
					}

					char buf[100];
					//sprintf(buf, "%3.6f", path.m_vecPathNodes[i].m_dVelocityLon);
					//sprintf(buf, "%3.6f", path.m_vecPathNodes[i].m_dVelocityLat);
				}
			}
		}
*/
		// Perform calculations
		for (int i = 0; i < calc.size(); i++) {
			AnnounceStartBlock("Calculating \"%s\"",
				calc.GetArgumentString(i).c_str());

			const ArgumentTree * pargtree = calc.GetSubTree(i);
			if (pargtree == NULL) {
				AnnounceEndBlock("WARNING: No operation");
				continue;
			}

			// Only assignment and function evaluations available
			if ((pargtree->size() <= 2) || (pargtree->size() >= 5)) {
				_EXCEPTIONT("Syntax error: Unable to interpret calculation");
			}
			if ((*pargtree)[1] != "=") {
				_EXCEPTIONT("Syntax error: Unable to interpret calculation");
			}

			// Assignment operation
			if (pargtree->size() == 3) {
				int ix = cdhInput.GetIndexFromString((*pargtree)[2]);
				if (ix == (-1)) {
					_EXCEPTION1("Unknown column header \"%s\"", (*pargtree)[2].c_str());
				}
				cdhInput.push_back((*pargtree)[0]);
				pathvec.Duplicate(ix);
				AnnounceEndBlock("Done");
				continue;
			}

			// Get the function arguments
			const ArgumentTree * pargfunc = pargtree->GetSubTree(3);
			if (pargfunc == NULL) {
				_EXCEPTIONT("Logic error");
			}

			// radial_wind_profile
			if ((*pargtree)[2] == "radial_wind_profile") {
				if (pargfunc->size() != 4) {
					_EXCEPTIONT("Syntax error: Function \"radial_wind_profile\" "
						"requires four arguments:\n"
						"radial_wind_profile(<u variable>, <v variable>, <# bins>, <bin width>)");
				}

				// Parse zonal wind variable
				Variable varU;
				varU.ParseFromString(varreg, (*pargfunc)[0]);
				VariableIndex varixU = varreg.FindOrRegister(varU);

				// Parse meridional wind variable
				Variable varV;
				varV.ParseFromString(varreg, (*pargfunc)[1]);
				VariableIndex varixV = varreg.FindOrRegister(varV);

				// Loop through all Times
				TimeToPathNodeMap::iterator iterPathNode =
					mapTimeToPathNode.begin();
				for (; iterPathNode != mapTimeToPathNode.end(); iterPathNode++) {
					const Time & time = iterPathNode->first;

					// Unload data from the VariableRegistry
					varreg.UnloadAllGridData();

					// Open NetCDF files with data at this time
					NcFileVector vecncDataFiles;
					int iTime;
					autocurator.Find(time, vecncDataFiles, iTime);
					if (vecncDataFiles.size() == 0) {
						_EXCEPTION1("Time (%s) does not exist in input data fileset",
							time.ToString().c_str());
					}

					// Loop through all PathNodes which need calculating at this Time
					for (int i = 0; i < iterPathNode->second.size(); i++) {
						int iPath = iterPathNode->second[i].first;
						int iPathNode = iterPathNode->second[i].second;

						PathNode & pathnode =
							pathvec[iPath].m_vecPathNodes[iPathNode];

						CalculateRadialProfile(
							varreg,
							vecncDataFiles,
							grid,
							cdhInput,
							iTime,
							pathnode,
							varixU,
							varixV,
							(*pargfunc)[2],
							(*pargfunc)[3]);
					}
				}

				// Add new variable to ColumnDataHeader
				cdhInput.push_back((*pargtree)[0]);

				AnnounceEndBlock("Done");
				continue;
			}

			// lastwhere
			if ((*pargtree)[2] == "lastwhere") {
				if (pargfunc->size() != 3) {
					_EXCEPTIONT("Syntax error: Function \"lastwhere\" "
						"requires three arguments:\n"
						"lastwhere(<column name>, <op>, <threshold>)");
				}

				// Get arguments
				int ix = cdhInput.GetIndexFromString((*pargfunc)[0]);
				if (ix == (-1)) {
					_EXCEPTION1("Invalid column header \"%s\"", (*pargfunc)[0].c_str());
				}

				const std::string & strOp = (*pargfunc)[1];

				const std::string & strThreshold = (*pargfunc)[2];

				// Loop through all PathNodes
				for (int p = 0; p < pathvec.size(); p++) {
					Path & path = pathvec[p];
					for (int i = 0; i < pathvec[p].m_vecPathNodes.size(); i++) {
						PathNode & pathnode = path.m_vecPathNodes[i];

						double dThreshold =
							pathnode.GetColumnDataAsDouble(cdhInput, strThreshold);

						ColumnDataRadialVelocityProfile * pdat =
							dynamic_cast<ColumnDataRadialVelocityProfile *>(
								pathnode.m_vecColumnData[ix]);

						if (pdat == NULL) {
							_EXCEPTION1("Cannot cast \"%s\" to RadialVelocityProfile type",
								(*pargfunc)[0].c_str());
						}

						const std::vector<double> & dArray = pdat->m_dUa;

						// Find array index
						int j = dArray.size()-1;
						if (strOp == ">=") {
							for (; j > 0; j--) {
								if (dArray[j] >= dThreshold) {
									break;
								}
							}

						} else if (strOp == ">") {
							for (; j > 0; j--) {
								if (dArray[j] > dThreshold) {
									break;
								}
							}

						} else if (strOp == "<=") {
							for (; j > 0; j--) {
								if (dArray[j] <= dThreshold) {
									break;
								}
							}

						} else if (strOp == "<") {
							for (; j > 0; j--) {
								if (dArray[j] < dThreshold) {
									break;
								}
							}

						} else {
							_EXCEPTION1("Invalid operator \"%s\" in function lastwhere()",
								strOp.c_str());
						}

						// Add this data to the pathnode
						pathnode.PushColumnData(
							new ColumnDataDouble(pdat->m_dR[j]));
					}
				}

				// Add new variable to ColumnDataHeader
				cdhInput.push_back((*pargtree)[0]);

				AnnounceEndBlock("Done");
				continue;
			}
/*
			// sum_radius
			if ((*pargtree)[2] == "sum_radius") {
				if (pargfunc->size() != 2) {
					_EXCEPTIONT("Syntax error: Function \"sum_radius\" "
						"requires two arguments:\n"
						"lastwhere(<field>, <radius>)");
				}

				// Parse variable
				Variable var;
				var.ParseFromString(varreg, (*pargfunc)[0]);
				VariableIndex varix = varreg.FindOrRegister(var);

				// Loop through all Times
				TimeToPathNodeMap::iterator iterPathNode =
					mapTimeToPathNode.begin();
				for (; iterPathNode != mapTimeToPathNode.end(); iterPathNode++) {
					const Time & time = iterPathNode->first;

					// Unload data from the VariableRegistry
					varreg.UnloadAllGridData();

					// Open NetCDF files with data at this time
					NcFileVector vecncDataFiles;
					int iTime;
					autocurator.Find(time, vecncDataFiles, iTime);
					if (vecncDataFiles.size() == 0) {
						_EXCEPTION1("Time (%s) does not exist in input data fileset",
							time.ToString().c_str());
					}

					// Loop through all PathNodes which need calculating at this Time
					for (int i = 0; i < iterPathNode->second.size(); i++) {
						int iPath = iterPathNode->second[i].first;
						int iPathNode = iterPathNode->second[i].second;

						PathNode & pathnode =
							pathvec[iPath].m_vecPathNodes[iPathNode];

						SumRadius(
							varreg,
							vecncDataFiles,
							grid,
							cdhInput,
							iTime,
							pathnode,
							varix,
							(*pargfunc)[1]);
					}
				}

				// Add new variable to ColumnDataHeader
				cdhInput.push_back((*pargtree)[0]);

				AnnounceEndBlock("Done");
				continue;
			}
*/
			// Unknown operation
			Announce(NULL);
			AnnounceEndBlock("WARNING: Unknown function \"%s\" no operation performed",
				(*pargtree)[2].c_str());

		}

		// Output
		if (strOutputFile != "") {
			std::vector<int> vecColumnDataOutIx;
			for (int i = 0; i < cdhOutput.size(); i++) {
				int ix = cdhInput.GetIndexFromString(cdhOutput[i]);
				if (ix == (-1)) {
					_EXCEPTION1("Unknown column data header \"%s\"",
						cdhOutput[i].c_str());
				} else {
					vecColumnDataOutIx.push_back(ix);
				}
			}

			AnnounceStartBlock("Writing output");
			FILE * fpOutput = fopen(strOutputFile.c_str(),"w");

			if (iftype == InputFileTypeSN) {
				for (int p = 0; p < pathvec.size(); p++) {
					Path & path = pathvec[p];
					fprintf(fpOutput, "start\t%i\t%i\t%i\t%i\t%i\n",
						static_cast<int>(path.m_vecPathNodes.size()),
						path.m_timeStart.GetYear(),
						path.m_timeStart.GetMonth(),
						path.m_timeStart.GetDay(),
						path.m_timeStart.GetSecond() / 3600);

					for (int i = 0; i < pathvec[p].m_vecPathNodes.size(); i++) {
						PathNode & pathnode = path.m_vecPathNodes[i];

						if (grid.m_nGridDim.size() == 1) {
							fprintf(fpOutput, "\t%lu", pathnode.m_gridix);
						} else if (grid.m_nGridDim.size() == 2) {
							fprintf(fpOutput, "\t%lu\t%lu",
								pathnode.m_gridix % grid.m_nGridDim[1],
								pathnode.m_gridix / grid.m_nGridDim[1]);
						}

						for (int j = 0; j < vecColumnDataOutIx.size(); j++) {
							const ColumnData * pcd =
								pathnode.m_vecColumnData[vecColumnDataOutIx[j]];
							fprintf(fpOutput, "\t%s", pcd->ToString().c_str());
						}

						fprintf(fpOutput, "\t%i\t%i\t%i\t%i\n",
							pathnode.m_time.GetYear(),
							pathnode.m_time.GetMonth(),
							pathnode.m_time.GetDay(),
							pathnode.m_time.GetSecond() / 3600);
					}
				}

			} else {
				_EXCEPTIONT("Sorry, not yet implemented!");
			}
			AnnounceEndBlock("Done");
		}
		AnnounceEndBlock("Done");
	}

	AnnounceBanner();

} catch(Exception & e) {
	Announce(e.ToString().c_str());
}

#if defined(TEMPEST_MPIOMP)
	// Deinitialize MPI
	MPI_Finalize();
#endif
}
