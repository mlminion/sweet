/*
 * rexi_swe.hpp
 *
 *  Created on: 24 Jul 2015
 *      Author: Martin Schreiber <schreiberx@gmail.com>
 */
#include "SWE_Plane_REXI.hpp"

#include <sweet/sweetmath.hpp>


#include <sweet/plane/PlaneDataComplex.hpp>
#include <sweet/plane/PlaneOperatorsComplex.hpp>


#include <sweet/plane/Convert_PlaneData_to_PlaneDataComplex.hpp>
#include <sweet/plane/Convert_PlaneDataComplex_to_PlaneData.hpp>

#ifndef SWEET_REXI_THREAD_PARALLEL_SUM
#	define SWEET_REXI_THREAD_PARALLEL_SUM 1
#endif

/**
 * Compute the REXI sum massively parallel *without* a parallelization with parfor in space
 */
#if SWEET_REXI_THREAD_PARALLEL_SUM
#	include <omp.h>
#endif


#ifndef SWEET_MPI
#	define SWEET_MPI 1
#endif


#if SWEET_MPI
#	include <mpi.h>
#endif


SWE_Plane_REXI::SWE_Plane_REXI()	:
	planeDataConfig(nullptr)
{
#if !SWEET_USE_LIBFFT
	std::cerr << "Spectral space required for solvers, use compile option --libfft=enable" << std::endl;
	exit(-1);
#endif


#if SWEET_REXI_THREAD_PARALLEL_SUM

	num_local_rexi_par_threads = omp_get_max_threads();

	if (num_local_rexi_par_threads == 0)
	{
		std::cerr << "FATAL ERROR: omp_get_max_threads == 0" << std::endl;
		exit(-1);
	}
#else
	num_local_rexi_par_threads = 1;
#endif

#if SWEET_MPI
	MPI_Comm_rank(MPI_COMM_WORLD, &mpi_rank);
	MPI_Comm_size(MPI_COMM_WORLD, &num_mpi_ranks);
#else
	mpi_rank = 0;
	num_mpi_ranks = 1;
#endif

	num_global_threads = num_local_rexi_par_threads * num_mpi_ranks;
}



void SWE_Plane_REXI::cleanup()
{
	for (std::vector<PerThreadVars*>::iterator iter = perThreadVars.begin(); iter != perThreadVars.end(); iter++)
	{
		PerThreadVars* p = *iter;
		delete p;
	}

	perThreadVars.resize(0);
}



SWE_Plane_REXI::~SWE_Plane_REXI()
{
	cleanup();

#if SWEET_BENCHMARK_REXI
	if (mpi_rank == 0)
	{
		std::cout << "STOPWATCH broadcast: " << stopwatch_broadcast() << std::endl;
		std::cout << "STOPWATCH preprocessing: " << stopwatch_preprocessing() << std::endl;
		std::cout << "STOPWATCH reduce: " << stopwatch_reduce() << std::endl;
		std::cout << "STOPWATCH solve_rexi_terms: " << stopwatch_solve_rexi_terms() << std::endl;
	}
#endif
}



/**
 * setup the REXI
 */
void SWE_Plane_REXI::setup(
		double i_h,						///< sampling size
		int i_M,						///< number of sampling points
		int i_L,						///< number of sampling points for Gaussian approx.
										///< set to 0 for auto detection

		PlaneDataConfig *i_planeDataConfig,
		const double *i_domain_size,	///< size of domain

		bool i_rexi_half,				///< use half-pole reduction
		bool i_rexi_normalization
)
{
	planeDataConfig = i_planeDataConfig;

	M = i_M;
	h = i_h;

	domain_size[0] = i_domain_size[0];
	domain_size[1] = i_domain_size[1];

	rexi.setup(0, h, M, i_L, i_rexi_half, i_rexi_normalization);

	std::size_t N = rexi.alpha.size();
	block_size = N/num_global_threads;
	if (block_size*num_global_threads != N)
		block_size++;

	cleanup();

	perThreadVars.resize(num_local_rexi_par_threads);

	/**
	 * We split the setup from the utilization here.
	 *
	 * This is necessary, since it has to be assured that
	 * the FFTW plans are initialized before using them.
	 */
	if (num_local_rexi_par_threads == 0)
	{
		std::cerr << "FATAL ERROR B: omp_get_max_threads == 0" << std::endl;
		exit(-1);
	}

#if SWEET_THREADING || SWEET_REXI_THREAD_PARALLEL_SUM
	if (omp_in_parallel())
	{
		std::cerr << "FATAL ERROR X: in parallel region" << std::endl;
		exit(-1);
	}
#endif

	PlaneDataConfig *planeDataConfig_local = this->planeDataConfig;

	// use a kind of serialization of the input to avoid threading conflicts in the ComplexFFT generation
	for (int j = 0; j < num_local_rexi_par_threads; j++)
	{
#if SWEET_REXI_THREAD_PARALLEL_SUM
#	pragma omp parallel for schedule(static,1) default(none) shared(planeDataConfig_local, i_domain_size,std::cout,j)
#endif
		for (int i = 0; i < num_local_rexi_par_threads; i++)
		{
			if (i != j)
				continue;

#if SWEET_THREADING || SWEET_REXI_THREAD_PARALLEL_SUM
			if (omp_get_thread_num() != i)
			{
				// leave this dummy std::cout in it to avoid the intel compiler removing this part
				std::cout << "ERROR: thread " << omp_get_thread_num() << " number mismatch " << i << std::endl;
				exit(-1);
			}
#endif

			perThreadVars[i] = new PerThreadVars;

			perThreadVars[i]->op.setup(planeDataConfig_local, i_domain_size);

			perThreadVars[i]->eta.setup(planeDataConfig_local);
			perThreadVars[i]->eta0.setup(planeDataConfig_local);
			perThreadVars[i]->u0.setup(planeDataConfig_local);
			perThreadVars[i]->v0.setup(planeDataConfig_local);
			perThreadVars[i]->h_sum.setup(planeDataConfig_local);
			perThreadVars[i]->u_sum.setup(planeDataConfig_local);
			perThreadVars[i]->v_sum.setup(planeDataConfig_local);
		}
	}

	if (num_local_rexi_par_threads == 0)
	{
		std::cerr << "FATAL ERROR C: omp_get_max_threads == 0" << std::endl;
		exit(-1);
	}

	for (int i = 0; i < num_local_rexi_par_threads; i++)
	{
		if (perThreadVars[i]->op.diff_c_x.physical_space_data == nullptr)
		{
			std::cerr << "ARRAY NOT INITIALIZED!!!!" << std::endl;
			exit(-1);
		}
	}

#if SWEET_REXI_THREAD_PARALLEL_SUM
#	pragma omp parallel for schedule(static,1) default(none)  shared(planeDataConfig_local, i_domain_size,std::cout)
#endif
	for (int i = 0; i < num_local_rexi_par_threads; i++)
	{
#if SWEET_THREADING || SWEET_REXI_THREAD_PARALLEL_SUM
//		int global_thread_id = omp_get_thread_num() + mpi_rank*num_local_rexi_par_threads;
		if (omp_get_thread_num() != i)
		{
			// leave this dummy std::cout in it to avoid the intel compiler removing this part
			std::cout << "ERROR: thread " << omp_get_thread_num() << " number mismatch " << i << std::endl;
			exit(-1);
		}
#else

//#if SWEET_REXI_THREAD_PARALLEL_SUM
//		int global_thread_id = 0;
//#endif

#endif

		if (perThreadVars[i]->eta.physical_space_data == nullptr)
		{
			std::cout << "ERROR, data == nullptr" << std::endl;
			exit(-1);
		}

		perThreadVars[i]->op.setup(planeDataConfig_local, i_domain_size);

		// initialize all values to account for first touch policy reason
		perThreadVars[i]->eta.spectral_set_all(0, 0);
		perThreadVars[i]->eta0.spectral_set_all(0, 0);

		perThreadVars[i]->u0.spectral_set_all(0, 0);
		perThreadVars[i]->v0.spectral_set_all(0, 0);

		perThreadVars[i]->h_sum.spectral_set_all(0, 0);
		perThreadVars[i]->u_sum.spectral_set_all(0, 0);
		perThreadVars[i]->v_sum.spectral_set_all(0, 0);

	}


#if SWEET_BENCHMARK_REXI
	stopwatch_preprocessing.reset();
	stopwatch_broadcast.reset();
	stopwatch_reduce.reset();
	stopwatch_solve_rexi_terms.reset();
#endif
}


/**
 * Solve SWE with implicit time stepping
 *
 * U_t = L U(0)
 *
 * (U(tau) - U(0)) / tau = L U(tau)
 *
 * <=> U(tau) - U(0) = L U(tau) tau
 *
 * <=> U(tau) - L tau U(tau) = U(0)
 *
 * <=> (1 - L tau) U(tau) = U(0)
 *
 * <=> (1/tau - L) U(tau) = U(0)/tau
 */
bool SWE_Plane_REXI::run_timestep_implicit_ts(
	PlaneData &io_h,
	PlaneData &io_u,
	PlaneData &io_v,

	double i_timestep_size,	///< timestep size

	PlaneOperators &op,
	const SimulationVariables &i_simVars
)
{
	PlaneDataComplex eta(io_h.planeDataConfig);

#if 0
	PlaneDataComplex eta0(io_h.planeDataConfig);
	PlaneDataComplex u0(io_u.planeDataConfig);
	PlaneDataComplex v0(io_v.planeDataConfig);

	eta0.loadRealFromPlaneData(io_h);
	u0.loadRealFromPlaneData(io_u);
	v0.loadRealFromPlaneData(io_v);
#else
	PlaneDataComplex eta0 = Convert_PlaneData_To_PlaneDataComplex::physical_convert(io_h);
	PlaneDataComplex u0 = Convert_PlaneData_To_PlaneDataComplex::physical_convert(io_u);
	PlaneDataComplex v0 = Convert_PlaneData_To_PlaneDataComplex::physical_convert(io_v);

#endif

	double alpha = 1.0/i_timestep_size;

	eta0 *= alpha;
	u0 *= alpha;
	v0 *= alpha;

	// load kappa (k)
	double kappa = alpha*alpha + i_simVars.sim.f0*i_simVars.sim.f0;

	double eta_bar = i_simVars.sim.h0;
	double g = i_simVars.sim.gravitation;

	assert(perThreadVars.size() != 0);
	assert(perThreadVars[0] != nullptr);
	PlaneOperatorsComplex &opc = perThreadVars[0]->op;

	PlaneDataComplex rhs =
			(kappa/alpha) * eta0
			- eta_bar*(opc.diff_c_x(u0) + opc.diff_c_y(v0))
			- (i_simVars.sim.f0*eta_bar/alpha) * (opc.diff_c_x(v0) - opc.diff_c_y(u0))
		;

	helmholtz_spectral_solver_spec(kappa, g*eta_bar, rhs, eta, 0);

	PlaneDataComplex uh = u0 - g*opc.diff_c_x(eta);
	PlaneDataComplex vh = v0 - g*opc.diff_c_y(eta);

	PlaneDataComplex u1 = alpha/kappa * uh     + i_simVars.sim.f0/kappa * vh;
	PlaneDataComplex v1 =    -i_simVars.sim.f0/kappa * uh + alpha/kappa * vh;

	io_h = Convert_PlaneDataComplex_To_PlaneData::physical_convert(eta);
	io_u = Convert_PlaneDataComplex_To_PlaneData::physical_convert(u1);
	io_v = Convert_PlaneDataComplex_To_PlaneData::physical_convert(v1);

	return true;
}


/**
 * Solve  SWE with Crank-Nicolson implicit time stepping
 *  (spectral formulation for Helmholtz eq) with semi-Lagrangian
 *   SL-SI-SP
 * U_t = L U(0)
 * Fully implicit version
 * (U(tau) - U(0)) / tau = 0.5*(L U(tau)+L U(0))
 *
 * <=> U(tau) - U(0) =  tau * 0.5*(L U(tau)+L U(0))
 *
 * <=> U(tau) - 0.5* L tau U(tau) = U(0) + tau * 0.5*L U(0)
 *
 * <=> (1 - 0.5 L tau) U(tau) = (1+tau*0.5*L) U(0)
 *
 * <=> (2/tau - L) U(tau) = (2/tau+L) U(0)
 *
 * Semi-implicit has coriolis term as totally explicit
 *
 *Semi-Lagrangian:
 *  U(tau) is on arrival points
 *  U(0) is on departure points
 *
 * Nonlinear term is added following Hortal (2002)
 *
 */
bool SWE_Plane_REXI::run_timestep_cn_sl_ts(
	PlaneData &io_h,  ///< Current and past fields
	PlaneData &io_u,
	PlaneData &io_v,
	PlaneData &io_h_prev,
	PlaneData &io_u_prev,
	PlaneData &io_v_prev,

	ScalarDataArray &i_posx_a, //Arrival point positions in x and y (this is basically the grid)
	ScalarDataArray &i_posy_a,

	double i_timestep_size,	///< timestep size
	int i_param_nonlinear, ///< degree of nonlinearity (0-linear, 1-full nonlinear, 2-only nonlinear adv)

	const SimulationVariables &i_simVars, ///< Parameters for simulation

	PlaneOperators &op,     ///< Operator class
	PlaneDataSampler &sampler2D, ///< Interpolation class
	SemiLagrangian &semiLagrangian  ///< Semi-Lag class
)
{

	//Out vars
	PlaneData h(io_h.planeDataConfig);
	PlaneData u(io_h.planeDataConfig);
	PlaneData v(io_h.planeDataConfig);

	//Departure points and arrival points

	ScalarDataArray posx_d = i_posx_a;
	ScalarDataArray posy_d = i_posy_a;

	//Parameters
	double h_bar = i_simVars.sim.h0;
	double g = i_simVars.sim.gravitation;
	double f0 = i_simVars.sim.f0;
	double dt = i_timestep_size;
	double alpha = 2.0/dt;
	double kappa = alpha*alpha;
	double kappa_bar = kappa;
	double stag_displacement[4] = {-0.5,-0.5,-0.5,-0.5}; //A grid staggering - centred cell
	kappa += f0*f0;
	kappa_bar -= f0*f0;

	if (i_param_nonlinear > 0)
	{
		//Calculate departure points
		semiLagrangian.semi_lag_departure_points_settls(
				io_u_prev, io_v_prev,
				io_u,	io_v,
				i_posx_a,	i_posy_a,
				dt,
				posx_d,	posy_d,
				stag_displacement
		);

	}

	//Calculate Divergence and vorticity spectrally
	PlaneData div = op.diff_c_x(io_u) + op.diff_c_y(io_v) ;
	//this could be pre-stored
	PlaneData div_prev = op.diff_c_x(io_u_prev) + op.diff_c_y(io_v_prev) ;

	//Calculate the RHS
	PlaneData rhs_u = alpha * io_u + f0 * io_v    - g * op.diff_c_x(io_h);
	PlaneData rhs_v =  - f0 * io_u + alpha * io_v - g * op.diff_c_y(io_h);
	PlaneData rhs_h = alpha * io_h  - h_bar * div;

	if (i_param_nonlinear > 0)
	{
		// all the RHS are to be evaluated at the departure points
		rhs_u=sampler2D.bicubic_scalar(rhs_u, posx_d, posy_d, -0.5, -0.5);
		rhs_v=sampler2D.bicubic_scalar(rhs_v, posx_d, posy_d, -0.5, -0.5);
		rhs_h=sampler2D.bicubic_scalar(rhs_h, posx_d, posy_d, -0.5, -0.5);

		//Get data in spectral space
		rhs_u.request_data_spectral();
		rhs_v.request_data_spectral();
		rhs_h.request_data_spectral();
	}

	//Calculate nonlinear term at half timestep and add to RHS of h eq.
	if (i_param_nonlinear == 1)
	{
		// Calculate nonlinear term interpolated to departure points
		// h*div is calculate in cartesian space (pseudo-spectrally)
		//div.aliasing_zero_high_modes();
		//div_prev.aliasing_zero_high_modes();
		PlaneData hdiv = 2.0 * io_h * div - io_h_prev * div_prev;
		//hdiv.aliasing_zero_high_modes();
		//std::cout<<offcent<<std::endl;
		PlaneData nonlin = 0.5 * io_h * div +
				0.5 * sampler2D.bicubic_scalar(hdiv, posx_d, posy_d, -0.5, -0.5);
		//add diffusion
		//nonlin.printSpectrumEnergy_y();
		//nonlin.printSpectrumIndex();

		//nonlin.aliasing_zero_high_modes();
		//nonlin.printSpectrumEnergy_y();
		//nonlin.printSpectrumIndex();
		//nonlin=diff(nonlin);
		//nonlin=op.implicit_diffusion(nonlin,i_simVars.sim.viscosity,i_simVars.sim.viscosity_order );
		//nonlin.printSpectrumIndex();
		//nonlin.aliasing_zero_high_modes();
		//nonlin.printSpectrumIndex();

		//std::cout << "blocked: "  << std::endl;
		//nonlin.printSpectrumEnergy();
		//std::cout << "Nonlinear error: " << nonlin.reduce_maxAbs() << std::endl;
		//std::cout << "Div: " << div.reduce_maxAbs() << std::endl;
		//nonlin=0;
		rhs_h = rhs_h - 2.0*nonlin;
		rhs_h.request_data_spectral();
	}


	//Build Helmholtz eq.
	PlaneData rhs_div = op.diff_c_x(rhs_u)+op.diff_c_y(rhs_v);
	PlaneData rhs_vort = op.diff_c_x(rhs_v)-op.diff_c_y(rhs_u);
	PlaneData rhs     = kappa* rhs_h / alpha - h_bar * rhs_div - f0 * h_bar * rhs_vort / alpha;

	// Helmholtz solver
	helmholtz_spectral_solver(kappa, g*h_bar, rhs, h, op);

	//Update u and v
	u = (1/kappa)*
			( alpha *rhs_u + f0 * rhs_v
					- g * alpha * op.diff_c_x(h)
					- g * f0 * op.diff_c_y(h))
					;

	v = (1/kappa)*
			( alpha *rhs_v - f0 * rhs_u
					+ g * f0 * op.diff_c_x(h)
					- g * alpha * op.diff_c_y(h))
					;


	//Set time (n) as time (n-1)
	io_h_prev=io_h;
	io_u_prev=io_u;
	io_v_prev=io_v;

	//output data
	io_h=h;
	io_u=u;
	io_v=v;

	return true;
}


/**
 * Solve  SWE with the novel Semi-Lagrangian Exponential Integrator
 *  SL-REXI
 *
 *  See documentation for details
 *
 */
bool SWE_Plane_REXI::run_timestep_slrexi(
	PlaneData &io_h,  ///< Current and past fields
	PlaneData &io_u,
	PlaneData &io_v,
	PlaneData &io_h_prev,
	PlaneData &io_u_prev,
	PlaneData &io_v_prev,

	ScalarDataArray &i_posx_a, //Arrival point positions in x and y (this is basically the grid)
	ScalarDataArray &i_posy_a,

	double i_timestep_size,	///< timestep size
	int i_param_nonlinear, ///< degree of nonlinearity (0-linear, 1-full nonlinear, 2-only nonlinear adv)

	bool i_linear_exp_analytical, //

	const SimulationVariables &i_simVars, ///< Parameters for simulation

	PlaneOperators &op,     ///< Operator class
	PlaneDataSampler &sampler2D, ///< Interpolation class
	SemiLagrangian &semiLagrangian  ///< Semi-Lag class
)
{
	//Out vars
	PlaneData h(io_h.planeDataConfig);
	PlaneData u(io_h.planeDataConfig);
	PlaneData v(io_h.planeDataConfig);
	PlaneData N_h(io_h.planeDataConfig);
	PlaneData N_u(io_h.planeDataConfig);
	PlaneData N_v(io_h.planeDataConfig);
	PlaneData hdiv(io_h.planeDataConfig);

	//Departure points and arrival points
	ScalarDataArray posx_d(io_h.planeDataConfig->physical_array_data_number_of_elements);
	ScalarDataArray posy_d(io_h.planeDataConfig->physical_array_data_number_of_elements);

	//Parameters
	double dt = i_timestep_size;
	double stag_displacement[4] = {-0.5,-0.5,-0.5,-0.5}; //A grid staggering - centred cell

	if (i_param_nonlinear > 0)
	{
		//Calculate departure points
		semiLagrangian.semi_lag_departure_points_settls(
				io_u_prev, io_v_prev,
				io_u,	io_v,
				i_posx_a,	i_posy_a,
				dt,
				posx_d,	posy_d,			// output
				stag_displacement
		);

	}

	u = io_u;
	v = io_v;
	h = io_h;

	N_u.physical_set_all(0);
	N_v.physical_set_all(0);
	N_h.physical_set_all(0);
	hdiv.physical_set_all(0);

	//Calculate nonlinear terms
	if(i_param_nonlinear==1)
	{
		//Calculate Divergence and vorticity spectrally
		hdiv =  - io_h * (op.diff_c_x(io_u) + op.diff_c_y(io_v));

		// Calculate nonlinear term for the previous time step
		// h*div is calculate in cartesian space (pseudo-spectrally)
		N_h =  - io_h_prev * (op.diff_c_x(io_u_prev) + op.diff_c_y(io_v_prev));

		//Calculate exp(Ldt)N(n-1), relative to previous timestep
		//Calculate the V{n-1} term as in documentation, with the exponential integrator
		if(i_linear_exp_analytical)
			run_timestep_direct_solution( N_h, N_u, N_v, dt, op, i_simVars );
		else
			run_timestep_rexi( N_h, N_u, N_v, dt, op, i_simVars);

		//Use N_h to store now the nonlinearity of the current time (prev will not be required anymore)
		//Update the nonlinear terms with the constants relative to dt
		N_u = -0.5 * dt * N_u; // N^n of u term is zero
		N_v = -0.5 * dt * N_v; // N^n of v term is zero
		N_h = dt * hdiv - 0.5 * dt * N_h ; //N^n of h has the nonlin term
	}



	if (i_param_nonlinear > 0)
	{
		//Build variables to be interpolated to dep. points
		// This is the W^n term in documentation
		u = u + N_u;
		v = v + N_v;
		h = h + N_h;

		// Interpolate W to departure points
		u = sampler2D.bicubic_scalar(u, posx_d, posy_d, -0.5, -0.5);
		v = sampler2D.bicubic_scalar(v, posx_d, posy_d, -0.5, -0.5);

		h = sampler2D.bicubic_scalar(h, posx_d, posy_d, -0.5, -0.5);

	}

	/*
	 * Calculate the exp(Ldt) W{n}_* term as in documentation, with the exponential integrator?
	 */
	if (i_linear_exp_analytical)
		run_timestep_direct_solution(h, u, v, dt, op, i_simVars);
	else
		run_timestep_rexi(h, u, v, dt, op, i_simVars);

	if (i_param_nonlinear == 1)
	{
		// Add nonlinearity in h
		h = h + 0.5 * dt * hdiv;
	}

	// Set time (n) as time (n-1)
	io_h_prev = io_h;
	io_u_prev = io_u;
	io_v_prev = io_v;

	// output data
	io_h = h;
	io_u = u;
	io_v = v;

	return true;
}


/**
 * Solve the REXI of \f$ U(t) = exp(L*t) \f$
 *
 * See
 * 		doc/rexi/understanding_rexi.pdf
 * for further information
 */
bool SWE_Plane_REXI::run_timestep_rexi(
	PlaneData &io_h,
	PlaneData &io_u,
	PlaneData &io_v,

	double i_timestep_size,	///< timestep size

	PlaneOperators &op,
	const SimulationVariables &i_parameters
)
{
	typedef std::complex<double> complex;

	std::size_t max_N = rexi.alpha.size();

	io_h.request_data_physical();
	io_u.request_data_physical();
	io_v.request_data_physical();

#if 0
	std::cout << io_h << std::endl;
	std::cout << std::endl;
	std::cout << io_u << std::endl;
	std::cout << std::endl;
	std::cout << io_v << std::endl;
	std::cout << std::endl;
	exit(1);
#endif

#if SWEET_MPI

#if SWEET_BENCHMARK_REXI
	if (mpi_rank == 0)
		stopwatch_broadcast.start();
#endif

	std::size_t data_size = io_h.planeDataConfig->physical_array_data_number_of_elements;
	MPI_Bcast(io_h.physical_space_data, data_size, MPI_DOUBLE, 0, MPI_COMM_WORLD);

	if (std::isnan(io_h.physical_get(0,0)))
		return false;


	MPI_Bcast(io_u.physical_space_data, data_size, MPI_DOUBLE, 0, MPI_COMM_WORLD);
	MPI_Bcast(io_v.physical_space_data, data_size, MPI_DOUBLE, 0, MPI_COMM_WORLD);

#if SWEET_BENCHMARK_REXI
	if (mpi_rank == 0)
		stopwatch_broadcast.stop();
#endif

#endif



#if SWEET_REXI_THREAD_PARALLEL_SUM
#	pragma omp parallel for schedule(static,1) default(none) shared(i_parameters, i_timestep_size, io_h, io_u, io_v, max_N, std::cout, std::cerr)
#endif
	for (int i = 0; i < num_local_rexi_par_threads; i++)
	{
#if SWEET_BENCHMARK_REXI
		bool stopwatch_measure = false;
	#if SWEET_REXI_THREAD_PARALLEL_SUM
		if (omp_get_thread_num() == 0)
	#endif
			if (mpi_rank == 0)
				stopwatch_measure = true;
#endif

#if SWEET_BENCHMARK_REXI
		if (stopwatch_measure)
			stopwatch_preprocessing.start();
#endif

		double eta_bar = i_parameters.sim.h0;
		double g = i_parameters.sim.gravitation;

		PlaneOperatorsComplex &opc = perThreadVars[i]->op;

		PlaneDataComplex &eta0 = perThreadVars[i]->eta0;
		PlaneDataComplex &u0 = perThreadVars[i]->u0;
		PlaneDataComplex &v0 = perThreadVars[i]->v0;

		PlaneDataComplex &h_sum = perThreadVars[i]->h_sum;
		PlaneDataComplex &u_sum = perThreadVars[i]->u_sum;
		PlaneDataComplex &v_sum = perThreadVars[i]->v_sum;

		PlaneDataComplex &eta = perThreadVars[i]->eta;


		/*
		 * INITIALIZATION - THIS IS THE NON-PARALLELIZABLE PART!
		 */
		h_sum.spectral_set_all(0, 0);
		u_sum.spectral_set_all(0, 0);
		v_sum.spectral_set_all(0, 0);

		eta0 = Convert_PlaneData_To_PlaneDataComplex::physical_convert(io_h);
		u0 = Convert_PlaneData_To_PlaneDataComplex::physical_convert(io_u);
		v0 = Convert_PlaneData_To_PlaneDataComplex::physical_convert(io_v);


		/**
		 * SPECTRAL SOLVER - DO EVERYTHING IN SPECTRAL SPACE
		 */
		// convert to spectral space
		// scale with inverse of tau
		eta0 = eta0*(1.0/i_timestep_size);
		u0 = u0*(1.0/i_timestep_size);
		v0 = v0*(1.0/i_timestep_size);

#if SWEET_REXI_THREAD_PARALLEL_SUM || SWEET_MPI

#if SWEET_THREADING || SWEET_REXI_THREAD_PARALLEL_SUM
		int local_thread_id = omp_get_thread_num();
#else
		int local_thread_id = 0;
#endif
		int global_thread_id = local_thread_id + num_local_rexi_par_threads*mpi_rank;

		std::size_t start = std::min(max_N, block_size*global_thread_id);
		std::size_t end = std::min(max_N, start+block_size);
#else
		std::size_t start = 0;
		std::size_t end = max_N;
#endif


		// reuse result from previous computations
		// this significantly speeds up the process
		// initial guess
//		eta.spectral_set_all(0,0);

		/*
		 * DO SUM IN PARALLEL
		 */

		// precompute a bunch of values
		// this would belong to a serial part according to Amdahls law
		//
		// (kappa + lhs_a)\eta = kappa/alpha*\eta_0 - (i_parameters.sim.f0*eta_bar/alpha) * rhs_b + rhs_a
		//
		PlaneDataComplex rhs_a = eta_bar*(opc.diff_c_x(u0) + opc.diff_c_y(v0));
		PlaneDataComplex rhs_b = (opc.diff_c_x(v0) - opc.diff_c_y(u0));

		PlaneDataComplex lhs_a = (-g*eta_bar)*(perThreadVars[i]->op.diff2_c_x + perThreadVars[i]->op.diff2_c_y);

#if SWEET_BENCHMARK_REXI
		if (stopwatch_measure)
			stopwatch_preprocessing.stop();
#endif

#if SWEET_BENCHMARK_REXI
		if (stopwatch_measure)
			stopwatch_solve_rexi_terms.start();
#endif

		for (std::size_t n = start; n < end; n++)
		{
			// load alpha (a) and scale by inverse of tau
			// we flip the sign to account for the -L used in exp(\tau (-L))
			complex alpha = DQStuff::convertComplex<double>(rexi.alpha[n])/i_timestep_size;
			complex beta = DQStuff::convertComplex<double>(rexi.beta_re[n]);

			// load kappa (k)
			complex kappa = alpha*alpha + i_parameters.sim.f0*i_parameters.sim.f0;

			/*
			 * TODO: we can even get more performance out of this operations
			 * by partly using the real Fourier transformation
			 */
			PlaneDataComplex rhs =
					(kappa/alpha) * eta0
					+ (-i_parameters.sim.f0*eta_bar/alpha) * rhs_b
					+ rhs_a
				;

			PlaneDataComplex lhs = lhs_a.spectral_addScalarAll(kappa);
//			rhs.spectral_div_element_wise(lhs, eta);
			eta = rhs.spectral_div_element_wise(lhs);

			PlaneDataComplex uh = u0 + g*opc.diff_c_x(eta);
			PlaneDataComplex vh = v0 + g*opc.diff_c_y(eta);

			PlaneDataComplex u1 = (alpha/kappa) * uh     - (i_parameters.sim.f0/kappa) * vh;
			PlaneDataComplex v1 = (i_parameters.sim.f0/kappa) * uh + (alpha/kappa) * vh;

			PlaneData tmp(h_sum.planeDataConfig);

			h_sum += eta*beta;
			u_sum += u1*beta;
			v_sum += v1*beta;
		}

#if SWEET_BENCHMARK_REXI
		if (stopwatch_measure)
			stopwatch_solve_rexi_terms.stop();
#endif
	}

#if SWEET_BENCHMARK_REXI
	if (mpi_rank == 0)
		stopwatch_reduce.start();
#endif

#if SWEET_REXI_THREAD_PARALLEL_SUM
	io_h.physical_set_all(0);
	io_u.physical_set_all(0);
	io_v.physical_set_all(0);

	for (int n = 0; n < num_local_rexi_par_threads; n++)
	{
		perThreadVars[n]->h_sum.request_data_physical();
		perThreadVars[n]->u_sum.request_data_physical();
		perThreadVars[n]->v_sum.request_data_physical();

		// sum real-valued elements
		#pragma omp parallel for schedule(static)
		for (std::size_t i = 0; i < io_h.planeDataConfig->physical_array_data_number_of_elements; i++)
			io_h.physical_space_data[i] += perThreadVars[n]->h_sum.physical_space_data[i].real();

		#pragma omp parallel for schedule(static)
		for (std::size_t i = 0; i < io_h.planeDataConfig->physical_array_data_number_of_elements; i++)
			io_u.physical_space_data[i] += perThreadVars[n]->u_sum.physical_space_data[i].real();

		#pragma omp parallel for schedule(static)
		for (std::size_t i = 0; i < io_h.planeDataConfig->physical_array_data_number_of_elements; i++)
			io_v.physical_space_data[i] += perThreadVars[n]->v_sum.physical_space_data[i].real();
	}

#else

	io_h = Convert_PlaneDataComplex_To_PlaneData::physical_convert(perThreadVars[0]->h_sum);
	io_u = Convert_PlaneDataComplex_To_PlaneData::physical_convert(perThreadVars[0]->u_sum);
	io_v = Convert_PlaneDataComplex_To_PlaneData::physical_convert(perThreadVars[0]->v_sum);

#endif


#if SWEET_MPI
	PlaneData tmp(io_h.planeDataConfig);

	io_h.request_data_physical();
	int retval = MPI_Reduce(io_h.physical_space_data, tmp.physical_space_data, data_size, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
	if (retval != MPI_SUCCESS)
	{
		std::cerr << "MPI FAILED!" << std::endl;
		exit(1);
	}

	std::swap(io_h.physical_space_data, tmp.physical_space_data);

	io_u.request_data_physical();
	MPI_Reduce(io_u.physical_space_data, tmp.physical_space_data, data_size, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
	std::swap(io_u.physical_space_data, tmp.physical_space_data);

	io_v.request_data_physical();
	MPI_Reduce(io_v.physical_space_data, tmp.physical_space_data, data_size, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
	std::swap(io_v.physical_space_data, tmp.physical_space_data);
#endif


#if SWEET_BENCHMARK_REXI
	if (mpi_rank == 0)
		stopwatch_reduce.stop();
#endif

	return true;
}



inline std::complex<double> conj(const std::complex<double> &v)
{
	return std::complex<double>(v.real(), -v.imag());
}




/**
 * This method computes the analytical solution based on the given initial values.
 *
 * See Embid/Madja/1996, Terry/Beth/2014, page 16
 * and
 * 		doc/swe_solution_for_L/sympy_L_spec_decomposition.py
 * for the dimensionful formulation.
 *
 * Don't use this function to frequently, since it always computes
 * the required coefficients on-the-fly which is expensive.
 */
void SWE_Plane_REXI::run_timestep_direct_solution(
		PlaneData &io_h,
		PlaneData &io_u,
		PlaneData &io_v,

		double i_timestep_size,	///< timestep size

		PlaneOperators &op,
		const SimulationVariables &i_simVars
)
{
	typedef std::complex<double> complex;

	double eta_bar = i_simVars.sim.h0;
	double g = i_simVars.sim.gravitation;
	double f = i_simVars.sim.f0;
	complex I(0.0,1.0);

	PlaneDataComplex i_h = Convert_PlaneData_To_PlaneDataComplex::physical_convert(io_h);
	PlaneDataComplex i_u = Convert_PlaneData_To_PlaneDataComplex::physical_convert(io_u);
	PlaneDataComplex i_v = Convert_PlaneData_To_PlaneDataComplex::physical_convert(io_v);

	PlaneDataComplex o_h(io_h.planeDataConfig);
	PlaneDataComplex o_u(io_h.planeDataConfig);
	PlaneDataComplex o_v(io_h.planeDataConfig);

	double s0 = i_simVars.sim.domain_size[0];
	double s1 = i_simVars.sim.domain_size[1];

#if SWEET_USE_PLANE_SPECTRAL_SPACE
	o_h.spectral_space_data_valid = true;
	o_h.physical_space_data_valid = false;

	o_u.spectral_space_data_valid = true;
	o_u.physical_space_data_valid = false;

	o_v.spectral_space_data_valid = true;
	o_v.physical_space_data_valid = false;
#endif

	for (std::size_t ik1 = 0; ik1 < i_h.planeDataConfig->spectral_complex_data_size[1]; ik1++)
	{
		for (std::size_t ik0 = 0; ik0 < i_h.planeDataConfig->spectral_complex_data_size[0]; ik0++)
		{
			if (ik0 == i_h.planeDataConfig->spectral_complex_data_size[0]/2 || ik1 == i_h.planeDataConfig->spectral_complex_data_size[1]/2)
			{
				o_h.p_spectral_set(ik1, ik0, 0, 0);
				o_u.p_spectral_set(ik1, ik0, 0, 0);
				o_v.p_spectral_set(ik1, ik0, 0, 0);
			}

			complex U_hat[3];
			U_hat[0] = i_h.spectral_get(ik1, ik0);
			U_hat[1] = i_u.spectral_get(ik1, ik0);
			U_hat[2] = i_v.spectral_get(ik1, ik0);

			double k0, k1;
			if (ik0 < i_h.planeDataConfig->spectral_complex_data_size[0]/2)
				k0 = (double)ik0;
			else
				k0 = (double)((int)ik0-(int)i_h.planeDataConfig->spectral_complex_data_size[0]);

			if (ik1 < i_h.planeDataConfig->spectral_complex_data_size[1]/2)
				k1 = (double)ik1;
			else
				k1 = (double)((int)ik1-(int)i_h.planeDataConfig->spectral_complex_data_size[1]);

			/*
			 * dimensionful formulation
			 * see doc/swe_solution_for_L
			 */

			double H0 = eta_bar;

			//////////////////////////////////////
			// GENERATED CODE START
			//////////////////////////////////////
			complex eigenvalues[3];
			complex eigenvectors[3][3];

			if (k0 == 0 && k1 == 0)
			{
//					complex wg = std::sqrt((complex)f*f*s0*s0*s1*s1);

				eigenvalues[0] = 0.0;
				eigenvalues[1] = -1.0*f;
				eigenvalues[2] = f;

				eigenvectors[0][0] = 1.00000000000000;
				eigenvectors[0][1] = 0.0;
				eigenvectors[0][2] = 0.0;
				eigenvectors[1][0] = 0.0;
				eigenvectors[1][1] = -1.0*I;
				eigenvectors[1][2] = 1.00000000000000;
				eigenvectors[2][0] = 0.0;
				eigenvectors[2][1] = I;
				eigenvectors[2][2] = 1.00000000000000;
			}
			else if (k0 == 0)
			{
//					complex wg = std::sqrt((complex)s0*s0*(f*f*s1*s1 + 4.0*M_PI*M_PI*g*g*k1*k1));

				eigenvalues[0] = 0.0;
				eigenvalues[1] = -1.0*1.0/s1*std::sqrt((complex)4.0*M_PI*M_PI*H0*g*k1*k1 + f*f*s1*s1);
				eigenvalues[2] = -1.0*I*1.0/s1*std::sqrt((complex)-4.0*M_PI*M_PI*H0*g*k1*k1 - 1.0*f*f*s1*s1);

				eigenvectors[0][0] = (1.0/2.0)*I*1.0/M_PI*f*1.0/g*1.0/k1*s1;
				eigenvectors[0][1] = 1.00000000000000;
				eigenvectors[0][2] = 0.0;
				eigenvectors[1][0] = -2.0*M_PI*H0*k1/std::sqrt((complex)4.0*M_PI*M_PI*H0*g*k1*k1 + f*f*s1*s1);
				eigenvectors[1][1] = -1.0*I*f*s1/std::sqrt((complex)4.0*M_PI*M_PI*H0*g*k1*k1 + f*f*s1*s1);
				eigenvectors[1][2] = 1.00000000000000;
				eigenvectors[2][0] = 2.0*M_PI*H0*k1/std::sqrt((complex)4.0*M_PI*M_PI*H0*g*k1*k1 + f*f*s1*s1);
				eigenvectors[2][1] = I*f*s1/std::sqrt((complex)4.0*M_PI*M_PI*H0*g*k1*k1 + f*f*s1*s1);
				eigenvectors[2][2] = 1.00000000000000;
			}
			else if (k1 == 0)
			{
//					complex wg = std::sqrt((complex)s1*s1*(f*f*s0*s0 + 4.0*M_PI*M_PI*g*g*k0*k0));

				eigenvalues[0] = 0.0;
				eigenvalues[1] = -1.0*1.0/s0*std::sqrt((complex)4.0*M_PI*M_PI*H0*g*k0*k0 + f*f*s0*s0);
				eigenvalues[2] = -1.0*I*1.0/s0*std::sqrt((complex)-4.0*M_PI*M_PI*H0*g*k0*k0 - 1.0*f*f*s0*s0);

				eigenvectors[0][0] = -1.0/2.0*I*1.0/M_PI*f*1.0/g*1.0/k0*s0;
				eigenvectors[0][1] = 0.0;
				eigenvectors[0][2] = 1.00000000000000;
				eigenvectors[1][0] = 2.0*I*M_PI*H0*1.0/f*k0*1.0/s0;
				eigenvectors[1][1] = -1.0*I*1.0/f*1.0/s0*std::sqrt((complex)4.0*M_PI*M_PI*H0*g*k0*k0 + f*f*s0*s0);
				eigenvectors[1][2] = 1.00000000000000;
				eigenvectors[2][0] = 2.0*I*M_PI*H0*1.0/f*k0*1.0/s0;
				eigenvectors[2][1] = 1.0/f*1.0/s0*std::sqrt((complex)-4.0*M_PI*M_PI*H0*g*k0*k0 - 1.0*f*f*s0*s0);
				eigenvectors[2][2] = 1.00000000000000;
			}
			else
			{
//					complex K2 = M_PI*M_PI*k0*k0 + M_PI*M_PI*k1*k1;
				complex w = std::sqrt((complex)4.0*M_PI*M_PI*H0*g*k0*k0*s1*s1 + 4.0*M_PI*M_PI*H0*g*k1*k1*s0*s0 + f*f*s0*s0*s1*s1);

//					complex wg = std::sqrt((complex)f*f*s0*s0*s1*s1 + 4.0*M_PI*M_PI*g*g*k0*k0*s1*s1 + 4.0*M_PI*M_PI*g*g*k1*k1*s0*s0);
				eigenvalues[0] = 0.0;
				eigenvalues[1] = -1.0*1.0/s0*1.0/s1*std::sqrt((complex)4.0*M_PI*M_PI*H0*g*k0*k0*s1*s1 + 4.0*M_PI*M_PI*H0*g*k1*k1*s0*s0 + f*f*s0*s0*s1*s1);
				eigenvalues[2] = -1.0*I*1.0/s0*1.0/s1*std::sqrt((complex)-4.0*M_PI*M_PI*H0*g*k0*k0*s1*s1 - 4.0*M_PI*M_PI*H0*g*k1*k1*s0*s0 - 1.0*f*f*s0*s0*s1*s1);

				eigenvectors[0][0] = -1.0/2.0*I*1.0/M_PI*f*1.0/g*1.0/k0*s0;
				eigenvectors[0][1] = -1.0*1.0/k0*k1*s0*1.0/s1;
				eigenvectors[0][2] = 1.00000000000000;
				eigenvectors[1][0] = 2.0*M_PI*H0*1.0/s0*1.0/w*(I*k0*s1*s1*(4.0*I*M_PI*M_PI*H0*g*k0*k1 + f*w) - 1.0*k1*s0*s0*(4.0*M_PI*M_PI*H0*g*k1*k1 + f*f*s1*s1))*1.0/(4.0*M_PI*M_PI*H0*g*k1*k1 + f*f*s1*s1);
				eigenvectors[1][1] = 1.0/s0*s1*1.0/(4.0*M_PI*M_PI*H0*g*k1*k1 + f*f*s1*s1)*(4.0*M_PI*M_PI*H0*g*k0*k1 - 1.0*I*f*w);
				eigenvectors[1][2] = 1.00000000000000;
				eigenvectors[2][0] = -2.0*M_PI*H0*1.0/s0*1.0/w*(I*k0*s1*s1*(4.0*I*M_PI*M_PI*H0*g*k0*k1 - 1.0*f*w) - 1.0*k1*s0*s0*(4.0*M_PI*M_PI*H0*g*k1*k1 + f*f*s1*s1))*1.0/(4.0*M_PI*M_PI*H0*g*k1*k1 + f*f*s1*s1);
				eigenvectors[2][1] = 1.0/s0*s1*1.0/(4.0*M_PI*M_PI*H0*g*k1*k1 + f*f*s1*s1)*(4.0*M_PI*M_PI*H0*g*k0*k1 + I*f*w);
				eigenvectors[2][2] = 1.00000000000000;
			}




			//////////////////////////////////////
			// GENERATED CODE END
			//////////////////////////////////////


			if (f == 0)
			{
				/*
				 * override if f == 0, see ./sympy_L_spec_decomposition.py executed with LNr=4
				 */
				if (k0 != 0 || k1 != 0)
				{
					double K2 = K2;

					eigenvalues[0] = 0.0;
					eigenvalues[1] = -2.0*M_PI*sqrt(H0)*sqrt((double)g)*sqrt(k0*k0 + k1*k1);
					eigenvalues[2] = 2.0*M_PI*sqrt(H0)*sqrt((double)g)*sqrt(k0*k0 + k1*k1);

					eigenvectors[0][0] = 0.0;
					eigenvectors[0][1] = -1.0*k1/sqrt(k0*k0 + k1*k1);
					eigenvectors[0][2] = k0/sqrt(k0*k0 + k1*k1);
					eigenvectors[1][0] = -1.0*sqrt(H0)*sqrt(k0*k0 + k1*k1)/sqrt(H0*(k0*k0 + k1*k1) + g*k0*k0 + g*k1*k1);
					eigenvectors[1][1] = sqrt((double)g)*k0/sqrt(H0*(k0*k0 + k1*k1) + g*k0*k0 + g*k1*k1);
					eigenvectors[1][2] = sqrt((double)g)*k1/sqrt(H0*(k0*k0 + k1*k1) + g*k0*k0 + g*k1*k1);
					eigenvectors[2][0] = sqrt(H0)*sqrt(k0*k0 + k1*k1)/sqrt(H0*(k0*k0 + k1*k1) + g*k0*k0 + g*k1*k1);
					eigenvectors[2][1] = sqrt((double)g)*k0/sqrt(H0*(k0*k0 + k1*k1) + g*k0*k0 + g*k1*k1);
					eigenvectors[2][2] = sqrt((double)g)*k1/sqrt(H0*(k0*k0 + k1*k1) + g*k0*k0 + g*k1*k1);
				}
				else
				{

					eigenvalues[0] = 0.0;
					eigenvalues[1] = 0.0;
					eigenvalues[2] = 0.0;

					eigenvectors[0][0] = 1.00000000000000;
					eigenvectors[0][1] = 0.0;
					eigenvectors[0][2] = 0.0;
					eigenvectors[1][0] = 0.0;
					eigenvectors[1][1] = 1.00000000000000;
					eigenvectors[1][2] = 0.0;
					eigenvectors[2][0] = 0.0;
					eigenvectors[2][1] = 0.0;
					eigenvectors[2][2] = 1.00000000000000;
				}
			}


			/*
			 * Compute inverse of Eigenvectors.
			 * This generalizes to the case that the Eigenvectors are not orthonormal.
			 */
			complex eigenvectors_inv[3][3];

			eigenvectors_inv[0][0] =  (eigenvectors[1][1]*eigenvectors[2][2] - eigenvectors[1][2]*eigenvectors[2][1]);
			eigenvectors_inv[0][1] = -(eigenvectors[0][1]*eigenvectors[2][2] - eigenvectors[0][2]*eigenvectors[2][1]);
			eigenvectors_inv[0][2] =  (eigenvectors[0][1]*eigenvectors[1][2] - eigenvectors[0][2]*eigenvectors[1][1]);

			eigenvectors_inv[1][0] = -(eigenvectors[1][0]*eigenvectors[2][2] - eigenvectors[1][2]*eigenvectors[2][0]);
			eigenvectors_inv[1][1] =  (eigenvectors[0][0]*eigenvectors[2][2] - eigenvectors[0][2]*eigenvectors[2][0]);
			eigenvectors_inv[1][2] = -(eigenvectors[0][0]*eigenvectors[1][2] - eigenvectors[0][2]*eigenvectors[1][0]);

			eigenvectors_inv[2][0] =  (eigenvectors[1][0]*eigenvectors[2][1] - eigenvectors[1][1]*eigenvectors[2][0]);
			eigenvectors_inv[2][1] = -(eigenvectors[0][0]*eigenvectors[2][1] - eigenvectors[0][1]*eigenvectors[2][0]);
			eigenvectors_inv[2][2] =  (eigenvectors[0][0]*eigenvectors[1][1] - eigenvectors[0][1]*eigenvectors[1][0]);

			complex s = eigenvectors[0][0]*eigenvectors_inv[0][0] + eigenvectors[0][1]*eigenvectors_inv[1][0] + eigenvectors[0][2]*eigenvectors_inv[2][0];

			for (int j = 0; j < 3; j++)
				for (int i = 0; i < 3; i++)
					eigenvectors_inv[j][i] /= s;


			// check
			for (int j = 0; j < 3; j++)
			{
				for (int i = 0; i < 3; i++)
				{
					if (
							std::isnan(eigenvectors[j][i].real()) || std::isinf(eigenvectors[j][i].real()) != 0	||
							std::isnan(eigenvectors[j][i].imag()) || std::isinf(eigenvectors[j][i].imag()) != 0
					)
					{
						std::cerr << "Invalid number in Eigenvector " << j << " detected: " << eigenvectors[j][0] << ", " << eigenvectors[j][1] << ", " << eigenvectors[j][2] << std::endl;
					}

					if (
							std::isnan(eigenvectors_inv[j][i].real()) || std::isinf(eigenvectors_inv[j][i].real()) != 0	||
							std::isnan(eigenvectors_inv[j][i].imag()) || std::isinf(eigenvectors_inv[j][i].imag()) != 0
					)
					{
						std::cerr << "Invalid number in inverse of Eigenvector " << j << " detected: " << eigenvectors_inv[j][0] << ", " << eigenvectors_inv[j][1] << ", " << eigenvectors_inv[j][2] << std::endl;
					}
				}
			}

			/*
			 * Solve based on previously computed data.
			 * Note, that this data can be also precomputed and reused every time.
			 */
			complex UEV0_sp[3];
			for (int k = 0; k < 3; k++)
			{
				UEV0_sp[k] = {0, 0};
				for (int j = 0; j < 3; j++)
					UEV0_sp[k] += eigenvectors_inv[j][k] * U_hat[j];
			}

			complex omega[3];
			omega[0] = std::exp(-I*eigenvalues[0]*i_timestep_size);
			omega[1] = std::exp(-I*eigenvalues[1]*i_timestep_size);
			omega[2] = std::exp(-I*eigenvalues[2]*i_timestep_size);

			complex U_hat_sp[3];
			for (int k = 0; k < 3; k++)
			{
				U_hat_sp[k] = {0, 0};
				for (int j = 0; j < 3; j++)
					U_hat_sp[k] += eigenvectors[j][k] * omega[j] * UEV0_sp[j];
			}

			o_h.p_spectral_set(ik1, ik0, U_hat_sp[0]);
			o_u.p_spectral_set(ik1, ik0, U_hat_sp[1]);
			o_v.p_spectral_set(ik1, ik0, U_hat_sp[2]);
		}
	}

	io_h = Convert_PlaneDataComplex_To_PlaneData::physical_convert(o_h);
	io_u = Convert_PlaneDataComplex_To_PlaneData::physical_convert(o_u);
	io_v = Convert_PlaneDataComplex_To_PlaneData::physical_convert(o_v);
}



void SWE_Plane_REXI::run_timestep_direct_solution_geopotential_formulation(
		PlaneData &io_phi,	///< geopotential
		PlaneData &io_u,
		PlaneData &io_v,

		double i_timestep_size,	///< timestep size

		PlaneOperators &op,
		const SimulationVariables &i_simVars
)
{
	io_phi /= i_simVars.sim.gravitation;

	run_timestep_direct_solution(
			io_phi,
			io_u,
			io_v,
			i_timestep_size,
			op,
			i_simVars
	);

	io_phi *= i_simVars.sim.gravitation;
}

