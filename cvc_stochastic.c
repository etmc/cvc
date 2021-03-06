/****************************************************
 * cvc_stochastic.c
 *
 * Thu Jul 16 18:06:11 MEST 2009
 *
 * TODO: 
 * - solve the potential problem of having
 *   one (or several) local T=0
 * - include optional subtraction of diagonal
 *   correlations
 * - include calculation of connected part
 * - checks
 ****************************************************/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>
#ifdef MPI
#  include <mpi.h>
#endif
#include "ifftw.h"
#include <getopt.h>

#define MAIN_PROGRAM

#include "cvc_complex.h"
#include "cvc_linalg.h"
#include "global.h"
#include "cvc_geometry.h"
#include "cvc_utils.h"
#include "mpi_init.h"
#include "io.h"
#include "propagator_io.h"
#include "Q_phi.h"

void usage() {
  fprintf(stdout, "Code to perform light neutral contractions\n");
  fprintf(stdout, "Usage:    [options]\n");
  fprintf(stdout, "Options: -v verbose\n");
  fprintf(stdout, "         -g apply a random gauge transformation\n");
  fprintf(stdout, "         -f input filename [default cvc.input]\n");
#ifdef MPI
  MPI_Abort(MPI_COMM_WORLD, 1);
  MPI_Finalize();
#endif
  exit(0);
}


int main(int argc, char **argv) {
  
  int c, i, mu, nu;
  int count        = 0;
  int filename_set = 0;
  int dims[4]      = {0,0,0,0};
  int l_LX_at, l_LXstart_at;
  int x0, x1, x2, x3, ix;
  int sid;
  double *disc = (double*)NULL;
  double *work = (double*)NULL;
  double *disc_diag = (double*)NULL;
  double phase[4];
  int verbose = 0;
  int do_gt   = 0;
  char filename[100];
  double ratime, retime;
  double plaq;
  double spinor1[24], spinor2[24], U_[18];
  complex w, w1, psi1[4], psi2[4];
  FILE *ofs;

  fftw_complex *in=(fftw_complex*)NULL;

#ifdef MPI
  fftwnd_mpi_plan plan_p, plan_m;
  int *status;
#else
  fftwnd_plan plan_p, plan_m;
#endif

#ifdef MPI
  MPI_Init(&argc, &argv);
#endif

  while ((c = getopt(argc, argv, "h?vgf:")) != -1) {
    switch (c) {
    case 'v':
      verbose = 1;
      break;
    case 'g':
      do_gt = 1;
      break;
    case 'f':
      strcpy(filename, optarg);
      filename_set=1;
      break;
    case 'h':
    case '?':
    default:
      usage();
      break;
    }
  }

  /* set the default values */
  set_default_input_values();
  if(filename_set==0) strcpy(filename, "cvc.input");

  /* read the input file */
  read_input(filename);

  /* some checks on the input data */
  if((T_global == 0) || (LX==0) || (LY==0) || (LZ==0)) {
    if(g_proc_id==0) fprintf(stdout, "T and L's must be set\n");
    usage();
  }
  if(g_kappa == 0.) {
    if(g_proc_id==0) fprintf(stdout, "kappa should be > 0.n");
    usage();
  }

  /* initialize MPI parameters */
  mpi_init(argc, argv);
#ifdef MPI
  if((status = (int*)calloc(g_nproc, sizeof(int))) == (int*)NULL) {
    MPI_Abort(MPI_COMM_WORLD, 1);
    MPI_Finalize();
    exit(7);
  }
#endif

  /* initialize fftw */
  dims[0]=T_global; dims[1]=LX; dims[2]=LY; dims[3]=LZ;
#ifdef MPI
  plan_p = fftwnd_mpi_create_plan(g_cart_grid, 4, dims, FFTW_BACKWARD, FFTW_MEASURE);
  plan_m = fftwnd_mpi_create_plan(g_cart_grid, 4, dims, FFTW_FORWARD, FFTW_MEASURE);
  fftwnd_mpi_local_sizes(plan_p, &T, &Tstart, &l_LX_at, &l_LXstart_at, &FFTW_LOC_VOLUME);
#else
  plan_p = fftwnd_create_plan(4, dims, FFTW_BACKWARD, FFTW_MEASURE | FFTW_IN_PLACE);
  plan_m = fftwnd_create_plan(4, dims, FFTW_FORWARD,  FFTW_MEASURE | FFTW_IN_PLACE);
  T            = T_global;
  Tstart       = 0;
  l_LX_at      = LX;
  l_LXstart_at = 0;
  FFTW_LOC_VOLUME = T*LX*LY*LZ;
#endif
  fprintf(stdout, "# [%2d] fftw parameters:\n"\
                  "# [%2d] T            = %3d\n"\
		  "# [%2d] Tstart       = %3d\n"\
		  "# [%2d] l_LX_at      = %3d\n"\
		  "# [%2d] l_LXstart_at = %3d\n"\
		  "# [%2d] FFTW_LOC_VOLUME = %3d\n", 
		  g_cart_id, g_cart_id, T, g_cart_id, Tstart, g_cart_id, l_LX_at,
		  g_cart_id, l_LXstart_at, g_cart_id, FFTW_LOC_VOLUME);

#ifdef MPI
  if(T==0) {
    fprintf(stderr, "[%2d] local T is zero; exit\n", g_cart_id);
    MPI_Abort(MPI_COMM_WORLD, 1);
    MPI_Finalize();
    exit(2);
  }
#endif

  if(init_geometry() != 0) {
    fprintf(stderr, "ERROR from init_geometry\n");
#ifdef MPI
    MPI_Abort(MPI_COMM_WORLD, 1);
    MPI_Finalize();
#endif
    exit(1);
  }

  geometry();

  /* read the gauge field */
  alloc_gauge_field(&g_gauge_field, VOLUMEPLUSRAND);
  sprintf(filename, "%s.%.4d", gaugefilename_prefix, Nconf);
  if(g_cart_id==0) fprintf(stdout, "reading gauge field from file %s\n", filename);
  read_lime_gauge_field_doubleprec(filename);
#ifdef MPI
  xchange_gauge();
#endif

  /* measure the plaquette */
  plaquette(&plaq);
  if(g_cart_id==0) fprintf(stdout, "measured plaquette value: %25.16e\n", plaq);

  /* allocate memory for the spinor fields */
  no_fields = 2;
  g_spinor_field = (double**)calloc(no_fields, sizeof(double*));
  for(i=0; i<no_fields; i++) alloc_spinor_field(&g_spinor_field[i], VOLUMEPLUSRAND);

  /* allocate memory for the contractions */
  disc = (double*)calloc(8*VOLUME, sizeof(double));
  work = (double*)calloc(20*VOLUME, sizeof(double));
  if( (disc==(double*)NULL) || (work==(double*)NULL) ) {
    fprintf(stderr, "could not allocate memory for disc/work\n");
#ifdef MPI
    MPI_Abort(MPI_COMM_WORLD, 1);
    MPI_Finalize();
#endif
    exit(3);
  }
  for(ix=0; ix<8*VOLUME; ix++) disc[ix] = 0.;

  if(g_subtract == 1) {
    /* allocate memory for disc_diag */
    disc_diag = (double*)calloc(20*VOLUME, sizeof(double));
    if( disc_diag == (double*)NULL ) {
      fprintf(stderr, "could not allocate memory for disc_diag\n");
#ifdef MPI
      MPI_Abort(MPI_COMM_WORLD, 1);
      MPI_Finalize();
#endif
      exit(8);
    }
    for(ix=0; ix<20*VOLUME; ix++) disc_diag[ix] = 0.;
  }

  /* prepare Fourier transformation arrays */
  in  = (fftw_complex*)malloc(FFTW_LOC_VOLUME*sizeof(fftw_complex));
  if(in==(fftw_complex*)NULL) {    
#ifdef MPI
    MPI_Abort(MPI_COMM_WORLD, 1);
    MPI_Finalize();
#endif
    exit(4);
  }

  if(g_resume==1) { /* read current disc from file */
    sprintf(filename, ".outcvc_current.%.4d", Nconf);
    c = read_contraction(disc, &count, filename, 4);
    if( (g_subtract==1) && (c==0) ) {
      sprintf(filename, ".outcvc_diag_current.%.4d", Nconf);
      c = read_contraction(disc_diag, (int*)NULL, filename, 10);
    }
#ifdef MPI
    MPI_Gather(&c, 1, MPI_INT, status, 1, MPI_INT, 0, g_cart_grid);
    if(g_cart_id==0) {
      /* check the entries in status */
      for(i=0; i<g_nproc; i++) 
        if(status[i]!=0) { status[0] = 1; break; }
    }
    MPI_Bcast(status, 1, MPI_INT, 0, g_cart_grid);
    if(status[0]==1) {
      for(ix=0; ix<8*VOLUME; ix++) disc[ix] = 0.;
      count = 0;
    }
#else
    if(c != 0) {
      fprintf(stdout, "could not read current disc; start new\n");
      for(ix=0; ix<8*VOLUME; ix++) disc[ix] = 0.;
      if(g_subtract==1) for(ix=0; ix<20*VOLUME; ix++) disc_diag[ix] = 0.;
      count = 0;
    }
#endif
    if(g_cart_id==0) fprintf(stdout, "starting with count = %d\n", count);
  }  /* of g_resume ==  1 */
  
  /* start loop on source id.s */
  for(sid=g_sourceid; sid<=g_sourceid2; sid++) {

    /* read the new propagator */
/*    sprintf(filename, "%s.%.4d.%.2d", filename_prefix, Nconf, sid); */
    sprintf(filename, "source.%.4d.%.2d.inverted", Nconf, sid);
    if(format==0) {
      if(read_lime_spinor(g_spinor_field[1], filename, 0) != 0) break;
    }
    else if(format==1) {
      if(read_cmi(g_spinor_field[1], filename) != 0) break;
    }
    count++;
    
    xchange_field(g_spinor_field[1]); 

    /* calculate the source: apply Q_phi_tbc */
    Q_phi_tbc(g_spinor_field[0], g_spinor_field[1]);
    xchange_field(g_spinor_field[0]); 

/*
    sprintf(filename, "%s.source.%.2d", filename, g_cart_id);
    ofs = fopen(filename, "w");
    printf_spinor_field(g_spinor_field[0], ofs);
    fclose(ofs);
*/

    /* add new contractions to (existing) disc */
#ifdef MPI
    ratime = MPI_Wtime();
#else
    ratime = (double)clock() / CLOCKS_PER_SEC;
#endif
    for(ix=0; ix<VOLUME; ix++) {    /* loop on lattice sites */
      for(mu=0; mu<4; mu++) { /* loop on Lorentz index of the current */

        _cm_eq_cm_ti_co(U_, &g_gauge_field[_GGI(ix, mu)], &co_phase_up[mu]);

        /* first contribution */
        _fv_eq_cm_ti_fv(spinor1, U_, &g_spinor_field[1][_GSI(g_iup[ix][mu])]);
	_fv_eq_gamma_ti_fv(spinor2, mu, spinor1);
	_fv_mi_eq_fv(spinor2, spinor1);
	_co_eq_fv_dag_ti_fv(&w, &g_spinor_field[0][_GSI(ix)], spinor2);
	disc[_GJI(ix, mu)  ] -= 0.25 * w.re;
	disc[_GJI(ix, mu)+1] -= 0.25 * w.im;
        if(g_subtract==1) {
	  work[_GWI(mu,ix,VOLUME)  ] = -0.25 * w.re;
	  work[_GWI(mu,ix,VOLUME)+1] = -0.25 * w.im;
	}

        /* second contribution */
	_fv_eq_cm_dag_ti_fv(spinor1, U_, &g_spinor_field[1][_GSI(ix)]);
	_fv_eq_gamma_ti_fv(spinor2, mu, spinor1);
	_fv_pl_eq_fv(spinor2, spinor1);
	_co_eq_fv_dag_ti_fv(&w, &g_spinor_field[0][_GSI(g_iup[ix][mu])], spinor2);
	disc[_GJI(ix, mu)  ] -= 0.25 * w.re;
	disc[_GJI(ix, mu)+1] -= 0.25 * w.im;
        if(g_subtract==1) {
	  work[_GWI(mu,ix,VOLUME)  ] -= 0.25 * w.re;
	  work[_GWI(mu,ix,VOLUME)+1] -= 0.25 * w.im;
	}
      }
    }
#ifdef MPI
    retime = MPI_Wtime();
#else
    retime = (double)clock() / CLOCKS_PER_SEC;
#endif
    fprintf(stdout, "[%2d] contractions in %e seconds\n", g_cart_id, retime-ratime);

    if(g_subtract==1) {
      /* add current contribution to disc_diag */
      for(mu=0; mu<4; mu++) {
        for(i=0; i<4; i++) phase[i] = (double)(i==mu);
        memcpy((void*)in, (void*)&work[_GWI(mu,0,VOLUME)], 2*VOLUME*sizeof(double));
#ifdef MPI
        fftwnd_mpi(plan_m, 1, in, NULL, FFTW_NORMAL_ORDER);
#else
        fftwnd_one(plan_m, in, NULL);
#endif
        for(x0=0; x0<T; x0++) {
        for(x1=0; x1<LX; x1++) {
        for(x2=0; x2<LY; x2++) {
        for(x3=0; x3<LZ; x3++) {
	  ix = g_ipt[x0][x1][x2][x3];
	  w.re =  cos( M_PI * 
	    (phase[0]*(double)(Tstart+x0)/(double)T_global + phase[1]*(double)x1/(double)LX + 
	     phase[2]*(double)x2/(double)LY                + phase[3]*(double)x3/(double)LZ) );
	  w.im = -sin( M_PI * 
	    (phase[0]*(double)(Tstart+x0)/(double)T_global + phase[1]*(double)x1/(double)LX + 
	     phase[2]*(double)x2/(double)LY                + phase[3]*(double)x3/(double)LZ) );
	  _co_eq_co_ti_co(&w1, &in[ix], &w);
	  work[_GWI(4+mu,ix,VOLUME)  ] = w1.re;
	  work[_GWI(4+mu,ix,VOLUME)+1] = w1.im;
	}
	}
	}
	}

        memcpy((void*)in, (void*)&work[_GWI(mu,0,VOLUME)], 2*VOLUME*sizeof(double));
#ifdef MPI
        fftwnd_mpi(plan_p, 1, in, NULL, FFTW_NORMAL_ORDER);
#else
        fftwnd_one(plan_p, in, NULL);
#endif
        for(x0=0; x0<T; x0++) {
        for(x1=0; x1<LX; x1++) {
        for(x2=0; x2<LY; x2++) {
        for(x3=0; x3<LZ; x3++) {
	  ix = g_ipt[x0][x1][x2][x3];
	  w.re = cos( M_PI * 
	    (phase[0]*(double)(Tstart+x0)/(double)T_global + phase[1]*(double)x1/(double)LX + 
	     phase[2]*(double)x2/(double)LY                + phase[3]*(double)x3/(double)LZ) );
	  w.im = sin( M_PI * 
	    (phase[0]*(double)(Tstart+x0)/(double)T_global + phase[1]*(double)x1/(double)LX + 
	     phase[2]*(double)x2/(double)LY                + phase[3]*(double)x3/(double)LZ) );
	  _co_eq_co_ti_co(&w1, &in[ix], &w);
	  work[_GWI(mu,ix,VOLUME)  ] = w1.re;
	  work[_GWI(mu,ix,VOLUME)+1] = w1.im;
	}
	}
	}
	}
      }  /* of mu */
      for(ix=0; ix<VOLUME; ix++) {
        i=-1;
        for(mu=0; mu<4; mu++) {
        for(nu=mu; nu<4; nu++) {
	  i++;
	  _co_eq_co_ti_co(&w, (complex*)&work[_GWI(mu,ix,VOLUME)], (complex*)&work[_GWI(4+nu,ix,VOLUME)]);
	  disc_diag[_GWI(ix,i,10)  ] += w.re;
	  disc_diag[_GWI(ix,i,10)+1] += w.im;
	}
	}
      }
    } /* of g_subtract == 1 */

    /* save results for count = multiple of Nsave */
    if(count%Nsave == 0) {
#ifdef MPI
      ratime = MPI_Wtime();
#else
      ratime = (double)clock() / CLOCKS_PER_SEC;
#endif
      if(g_cart_id == 0) fprintf(stdout, "save results for count = %d\n", count);

      /* save the result in position space */
      sprintf(filename, "outcvc_X.%.4d.%.4d", Nconf, count);
      write_contraction(disc, NULL, filename, 4, 1, 0);

      /* Fourier transform data, copy to work */
      for(mu=0; mu<4; mu++) {
        for(i=0; i<4; i++) phase[i] = (double)(i==mu);
        for(ix=0; ix<VOLUME; ix++) {
	  in[ix].re = disc[_GJI(ix,mu)  ];
	  in[ix].im = disc[_GJI(ix,mu)+1];
	}
#ifdef MPI
        fftwnd_mpi(plan_m, 1, in, NULL, FFTW_NORMAL_ORDER);
#else
        fftwnd_one(plan_m, in, NULL);
#endif
        for(x0=0; x0<T; x0++) {
        for(x1=0; x1<LX; x1++) {
        for(x2=0; x2<LY; x2++) {
        for(x3=0; x3<LZ; x3++) {
	  ix = g_ipt[x0][x1][x2][x3];
	  w.re =  cos( M_PI * 
	    (phase[0]*(double)(Tstart+x0)/(double)T_global + phase[1]*(double)x1/(double)LX + 
	     phase[2]*(double)x2/(double)LY                + phase[3]*(double)x3/(double)LZ) );
	  w.im = -sin( M_PI * 
	    (phase[0]*(double)(Tstart+x0)/(double)T_global + phase[1]*(double)x1/(double)LX + 
	     phase[2]*(double)x2/(double)LY                + phase[3]*(double)x3/(double)LZ) );
	  _co_eq_co_ti_co(&w1, &in[ix], &w);
	  work[_GWI(ix,4+mu,8)  ] = w1.re / (double)count;
	  work[_GWI(ix,4+mu,8)+1] = w1.im / (double)count;
	}
	}
	}
	}

        for(ix=0; ix<VOLUME; ix++) {
	  in[ix].re = disc[_GJI(ix, mu)  ];
	  in[ix].im = disc[_GJI(ix, mu)+1];
	}
#ifdef MPI
        fftwnd_mpi(plan_p, 1, in, NULL, FFTW_NORMAL_ORDER);
#else
        fftwnd_one(plan_p, in, NULL);
#endif
        for(x0=0; x0<T; x0++) {
        for(x1=0; x1<LX; x1++) {
        for(x2=0; x2<LY; x2++) {
        for(x3=0; x3<LZ; x3++) {
	  ix = g_ipt[x0][x1][x2][x3];
	  w.re = cos( M_PI * 
	    (phase[0]*(double)(Tstart+x0)/(double)T_global + phase[1]*(double)x1/(double)LX + 
	     phase[2]*(double)x2/(double)LY                + phase[3]*(double)x3/(double)LZ) );
	  w.im = sin( M_PI * 
	    (phase[0]*(double)(Tstart+x0)/(double)T_global + phase[1]*(double)x1/(double)LX + 
	     phase[2]*(double)x2/(double)LY                + phase[3]*(double)x3/(double)LZ) );
	  _co_eq_co_ti_co(&w1, &in[ix], &w);
	  work[_GWI(ix,mu,8)  ] = w1.re / (double)count;
	  work[_GWI(ix,mu,8)+1] = w1.im / (double)count;
	}
	}
	}
	}
	 
      }  /* of mu =0 ,..., 3*/

      /* save the result in momentum space */
      sprintf(filename, "outcvc_P.%.4d.%.4d", Nconf, count);
      write_contraction(work, NULL, filename, 8, 1, 0);

      /* calculate the correlations 00, 01, 02, 03, 11, 12, ..., 23, 33 */
      for(ix=VOLUME-1; ix>=0; ix--) {
        /* copy current data to auxilliary vector */
	memcpy((void*)psi1, (void*)&work[_GWI(ix,0,8)], 8*sizeof(double));
	memcpy((void*)psi2, (void*)&work[_GWI(ix,4,8)], 8*sizeof(double));
	i = -1;
        for(mu=0; mu<4; mu++) {
        for(nu=mu; nu<4; nu++) {
	  i++;
          _co_eq_co_ti_co(&w,&psi1[mu],&psi2[nu]);
	  if(g_subtract !=1 ) {
            work[_GWI(ix,i,10)  ] = w.re / (double)(T_global*LX*LY*LZ);
            work[_GWI(ix,i,10)+1] = w.im / (double)(T_global*LX*LY*LZ);
	  }
	  else {
	    work[_GWI(ix,i,10)  ] =
	      ( w.re - disc_diag[_GWI(ix,i,10)  ]/(double)(count*count) ) / 
	        (double)(T_global*LX*LY*LZ);
	    work[_GWI(ix,i,10)+1] =
	      ( w.im - disc_diag[_GWI(ix,i,10)+1]/(double)(count*count) ) / 
	        (double)(T_global*LX*LY*LZ);
	  }
        }
	}
      }
 
      /* save current results to file */
      sprintf(filename, "outcvc_final.%.4d.%.4d", Nconf, count);
      write_contraction(work, (int*)NULL, filename, 10, 1, 0);
#ifdef MPI
      retime = MPI_Wtime();
#else
      retime = (double)clock() / CLOCKS_PER_SEC;
#endif
      fprintf(stdout, "[%2d] time to save results: %e seconds\n", g_cart_id, retime-ratime);
    }  /* of count % Nsave == 0 */

  }  /* of loop on sid */

  /* write current disc to file */
  sprintf(filename, ".outcvc_current.%.4d", Nconf);
  write_contraction(disc, &count, filename, 4, 0, 0);

  if(g_subtract == 1) {
    /* write current disc_diag to file */
    sprintf(filename, ".outcvc_diag_current.%.4d", Nconf);
    write_contraction(disc_diag, (int*)NULL, filename, 10, 0, 0);
  }

  /* free the allocated memory, finalize */
  free(g_gauge_field); g_gauge_field=(double*)NULL;
  for(i=0; i<no_fields; i++) {
    free(g_spinor_field[i]);
    g_spinor_field[i] = (double*)NULL;
  }
  free(g_spinor_field); g_spinor_field=(double**)NULL;
  free_geometry();
  fftw_free(in);
  free(disc);
  free(work);
  if(g_subtract==1) free(disc_diag);
#ifdef MPI
  fftwnd_mpi_destroy_plan(plan_p);
  fftwnd_mpi_destroy_plan(plan_m);
  free(status);
  MPI_Finalize();
#else
  fftwnd_destroy_plan(plan_p);
  fftwnd_destroy_plan(plan_m);
#endif

  return(0);

}
