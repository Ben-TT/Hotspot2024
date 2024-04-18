#include "include/traj.h"
void tnp(fields *fi, particles *pt, par *par)
{
   static bool fastIO;
   static bool first = true;

   //  create buffers on the device
   /** IMPORTANT: do not use CL_MEM_USE_HOST_PTR if on dGPU **/
   /** HOST_PTR is only used so that memory is not copied, but instead shared between CPU and iGPU in RAM**/
   // Note that special alignment has been given to Ea, Ba, y0, z0, x0, x1, y1 in order to actually do this properly

   // Assume buffers A, B, I, J (Ea, Ba, ci, cf) will always be the same. Then we save a bit of time.
   static cl::Buffer buff_Ea(context_g, (fastIO ? CL_MEM_USE_HOST_PTR : 0) | CL_MEM_READ_WRITE, n_cells3x8f, fastIO ? fi->Ea : NULL);
   static cl::Buffer buff_Ba(context_g, (fastIO ? CL_MEM_USE_HOST_PTR : 0) | CL_MEM_READ_WRITE, n_cells3x8f, fastIO ? fi->Ba : NULL);

   // cout << "command q" << endl; //  create queue to which we will push commands for the device.
   static cl::CommandQueue queue(context_g, default_device_g);
#if defined(sphere)
#if defined(octant)
   cl::Kernel kernel_tnp = cl::Kernel(program_g, "tnp_k_implicito"); // select the kernel program to run
#else
   cl::Kernel kernel_tnp = cl::Kernel(program_g, "tnp_k_implicit"); // select the kernel program to run
#endif
#endif

#if defined(cylinder)
   cl::Kernel kernel_tnp = cl::Kernel(program_g, "tnp_k_implicitz"); // select the kernel program to run
#endif

   cl::Kernel kernel_trilin = cl::Kernel(program_g, "trilin_k"); // select the kernel program to run
   cl::Kernel kernel_density = cl::Kernel(program_g, "density"); // select the kernel program to run
   cl::Kernel kernel_df = cl::Kernel(program_g, "df");           // select the kernel program to run
   cl::Kernel kernel_dtotal = cl::Kernel(program_g, "dtotal");   // ncalc_e = par->ncalcp[0];
                                                                 // ncalc_i = par->ncalcp[1];
#ifdef BFon_
   // check minus sign
   par->Bcoef[0] = -(float)qs[0] * e_charge_mass / (float)mp[0] * par->dt[0] * 0.5f;
   par->Bcoef[1] = -(float)qs[1] * e_charge_mass / (float)mp[1] * par->dt[1] * 0.5f;
#else
   par->Bcoef[0] = 0;
   par->Bcoef[1] = 0;
#endif
#ifdef EFon_
   par->Ecoef[0] = -(float)qs[0] * e_charge_mass / (float)mp[0] * par->dt[0] * 0.5f * par->dt[0]; // multiply by dt because of the later portion of cl code
   par->Ecoef[1] = -(float)qs[1] * e_charge_mass / (float)mp[1] * par->dt[1] * 0.5f * par->dt[1]; // multiply by dt because of the later portion of cl code
#else
   par->Ecoef[0] = 0;
   par->Ecoef[1] = 0;
#endif
   // cout << " Bconst=" << par->Bcoef[0] << ", Econst=" << par->Ecoef[0] << endl;
   if (first)
   { // get whether or not we are on an iGPU/similar, and can use certain memmory optimizations
      bool temp;
      default_device_g.getInfo(CL_DEVICE_HOST_UNIFIED_MEMORY, &temp);
      if (temp == true)
      { // is mapping required? // Yes we might need to map because OpenCL does not guarantee that the data will be shared, alternatively use SVM
         info_file << "Using unified memory: " << temp << " ";
      }
      else
      {
         info_file << "No unified memory: " << temp << " ";
      }
      fastIO = temp;
      fastIO = false;
      //  cout << "write buffer" << endl;
      queue.enqueueWriteBuffer(fi->buff_E[0], CL_TRUE, 0, n_cellsf * 3, fi->E);
      queue.enqueueWriteBuffer(fi->buff_B[0], CL_TRUE, 0, n_cellsf * 3, fi->B);

      queue.enqueueWriteBuffer(pt->buff_x0_e[0], CL_TRUE, 0, n_partf, pt->pos0x[0]);
      queue.enqueueWriteBuffer(pt->buff_y0_e[0], CL_TRUE, 0, n_partf, pt->pos0y[0]);
      queue.enqueueWriteBuffer(pt->buff_z0_e[0], CL_TRUE, 0, n_partf, pt->pos0z[0]);
      queue.enqueueWriteBuffer(pt->buff_x1_e[0], CL_TRUE, 0, n_partf, pt->pos1x[0]);
      queue.enqueueWriteBuffer(pt->buff_y1_e[0], CL_TRUE, 0, n_partf, pt->pos1y[0]);
      queue.enqueueWriteBuffer(pt->buff_z1_e[0], CL_TRUE, 0, n_partf, pt->pos1z[0]);

      queue.enqueueWriteBuffer(pt->buff_q_e[0], CL_TRUE, 0, n_partf, pt->q[0]);

      queue.enqueueWriteBuffer(pt->buff_x0_i[0], CL_TRUE, 0, n_partf, pt->pos0x[1]);
      queue.enqueueWriteBuffer(pt->buff_y0_i[0], CL_TRUE, 0, n_partf, pt->pos0y[1]);
      queue.enqueueWriteBuffer(pt->buff_z0_i[0], CL_TRUE, 0, n_partf, pt->pos0z[1]);
      queue.enqueueWriteBuffer(pt->buff_x1_i[0], CL_TRUE, 0, n_partf, pt->pos1x[1]);
      queue.enqueueWriteBuffer(pt->buff_y1_i[0], CL_TRUE, 0, n_partf, pt->pos1y[1]);
      queue.enqueueWriteBuffer(pt->buff_z1_i[0], CL_TRUE, 0, n_partf, pt->pos1z[1]);

      queue.enqueueWriteBuffer(pt->buff_q_i[0], CL_TRUE, 0, n_partf, pt->q[1]);
      //  fastIO = false;
   }

   int cdt;
   for (int ntime = 0; ntime < par->nc; ntime++)
   {
      // timer.mark();
      kernel_trilin.setArg(0, buff_Ea);                   // the 1st argument to the kernel program Ea
      kernel_trilin.setArg(1, fi->buff_E[0]);             // Ba
      kernel_trilin.setArg(2, sizeof(float), &par->a0_f); // scale
      // run the kernel
      queue.enqueueNDRangeKernel(kernel_trilin, cl::NullRange, cl::NDRange(n_cells), cl::NullRange);
      //  queue.finish(); // wait for the end of the kernel program
      kernel_trilin.setArg(0, buff_Ba);                   // the 1st argument to the kernel program Ea
      kernel_trilin.setArg(1, fi->buff_B[0]);             // Ba
      kernel_trilin.setArg(2, sizeof(float), &par->a0_f); // scale
      queue.enqueueNDRangeKernel(kernel_trilin, cl::NullRange, cl::NDRange(n_cells), cl::NullRange);
      // queue.finish();
      queue.enqueueFillBuffer(fi->buff_npi[0], 0, 0, n_cellsi);
      //    queue.finish();
      // queue.enqueueFillBuffer(buff_np_centeri, 0, 0, n_cellsi * 3);
      queue.enqueueFillBuffer(fi->buff_cji[0], 0, 0, n_cellsi * 3);
      // queue.enqueueFillBuffer(buff_cj_centeri, 0, 0, n_cellsi * 3 * 3);
      //   set arguments to be fed into the kernel program
      //   cout << "kernel arguments for electron" << endl;
      queue.finish(); // wait for trilinear to end before startin tnp electron
      //      cout << "\ntrilin " << timer.elapsed() << "s, \n";
      kernel_tnp.setArg(0, buff_Ea);                        // the 1st argument to the kernel program Ea
      kernel_tnp.setArg(1, buff_Ba);                        // Ba
      kernel_tnp.setArg(2, pt->buff_x0_e[0]);               // x0
      kernel_tnp.setArg(3, pt->buff_y0_e[0]);               // y0
      kernel_tnp.setArg(4, pt->buff_z0_e[0]);               // z0
      kernel_tnp.setArg(5, pt->buff_x1_e[0]);               // x1
      kernel_tnp.setArg(6, pt->buff_y1_e[0]);               // y1
      kernel_tnp.setArg(7, pt->buff_z1_e[0]);               // z1
      kernel_tnp.setArg(8, sizeof(float), &par->Bcoef[0]);  // Bconst
      kernel_tnp.setArg(9, sizeof(float), &par->Ecoef[0]);  // Econst
      kernel_tnp.setArg(10, sizeof(float), &par->a0_f);     // scale factor
      kernel_tnp.setArg(11, sizeof(int), &par->n_partp[0]); // npart
      kernel_tnp.setArg(12, sizeof(int), &par->ncalcp[0]);  // ncalc
      kernel_tnp.setArg(13, pt->buff_q_e[0]);               // q
      // cout << "run kernel_tnp for electron" << endl;
      //  timer.mark();
      queue.enqueueNDRangeKernel(kernel_tnp, cl::NullRange, cl::NDRange(par->n_part[1]), cl::NullRange);

      queue.finish();
      kernel_density.setArg(0, pt->buff_x0_e[0]);          // x0
      kernel_density.setArg(1, pt->buff_y0_e[0]);          // y0
      kernel_density.setArg(2, pt->buff_z0_e[0]);          // z0
      kernel_density.setArg(3, pt->buff_x1_e[0]);          // x1
      kernel_density.setArg(4, pt->buff_y1_e[0]);          // y1
      kernel_density.setArg(5, pt->buff_z1_e[0]);          // z1
      kernel_density.setArg(6, fi->buff_npi[0]);           // np integer temp
      kernel_density.setArg(7, fi->buff_cji[0]);           // current
      kernel_density.setArg(8, pt->buff_q_e[0]);           // q
      kernel_density.setArg(9, sizeof(float), &par->a0_f); // scale factor
      // queue.finish();

      //      cout << "\nelectron tnp " << timer.elapsed() << "s, \n";
      // wait for the end of the tnp electron to finish before starting density electron
      // run the kernel to get electron density
      //  timer.mark();
      queue.enqueueNDRangeKernel(kernel_density, cl::NullRange, cl::NDRange(par->n_part[1]), cl::NullRange);
      queue.finish();

      kernel_df.setArg(0, fi->buff_np_e[0]);          // np
      kernel_df.setArg(1, fi->buff_npi[0]);           // npt
      kernel_df.setArg(2, fi->buff_currentj_e[0]);    // current
      kernel_df.setArg(3, fi->buff_cji[0]);           // current
      kernel_df.setArg(4, sizeof(float), &par->a0_f); // scale factor

      queue.enqueueNDRangeKernel(kernel_df, cl::NullRange, cl::NDRange(n_cells), cl::NullRange);
      queue.finish();

      //  cout << "\nelectron density " << timer.elapsed() << "s, \n";
      // timer.mark();
      //  set arguments to be fed into the kernel program
      kernel_tnp.setArg(0, buff_Ea);                        // the 1st argument to the kernel program Ea
      kernel_tnp.setArg(1, buff_Ba);                        // Ba
      kernel_tnp.setArg(2, pt->buff_x0_i[0]);               // x0
      kernel_tnp.setArg(3, pt->buff_y0_i[0]);               // y0
      kernel_tnp.setArg(4, pt->buff_z0_i[0]);               // z0
      kernel_tnp.setArg(5, pt->buff_x1_i[0]);               // x1
      kernel_tnp.setArg(6, pt->buff_y1_i[0]);               // y1
      kernel_tnp.setArg(7, pt->buff_z1_i[0]);               // z1
      kernel_tnp.setArg(8, sizeof(float), &par->Bcoef[1]);  // Bconst
      kernel_tnp.setArg(9, sizeof(float), &par->Ecoef[1]);  // Econst
      kernel_tnp.setArg(10, sizeof(float), &par->a0_f);     // scale factor
      kernel_tnp.setArg(11, sizeof(int), &par->n_partp[1]); // npart
      kernel_tnp.setArg(12, sizeof(int), &par->ncalcp[1]);  //
      kernel_tnp.setArg(13, pt->buff_q_i[0]);               // q

      // cout << "run kernel for ions" << endl;
      queue.enqueueNDRangeKernel(kernel_tnp, cl::NullRange, cl::NDRange(par->n_part[1]), cl::NullRange);

      queue.enqueueFillBuffer(fi->buff_npi[0], 0, 0, n_cellsi);
      queue.enqueueFillBuffer(fi->buff_cji[0], 0, 0, n_cellsi * 3);

      queue.finish(); // wait for the tnp for ions to finish before

      kernel_density.setArg(0, pt->buff_x0_i[0]);          // x0
      kernel_density.setArg(1, pt->buff_y0_i[0]);          // y0
      kernel_density.setArg(2, pt->buff_z0_i[0]);          // z0
      kernel_density.setArg(3, pt->buff_x1_i[0]);          // x1
      kernel_density.setArg(4, pt->buff_y1_i[0]);          // y1
      kernel_density.setArg(5, pt->buff_z1_i[0]);          // z1
      kernel_density.setArg(6, fi->buff_npi[0]);           // np temp integer
      kernel_density.setArg(7, fi->buff_cji[0]);           // current
      kernel_density.setArg(8, pt->buff_q_i[0]);           // q
      kernel_density.setArg(9, sizeof(float), &par->a0_f); // scale factor

      // wait for the end of the tnp ion to finish before starting density ion
      // run the kernel to get ion density
      queue.enqueueNDRangeKernel(kernel_density, cl::NullRange, cl::NDRange(par->n_part[1]), cl::NullRange);
      queue.finish();
      kernel_df.setArg(0, fi->buff_np_i[0]);          // np ion
      kernel_df.setArg(1, fi->buff_npi[0]);           // np ion temp integer
      kernel_df.setArg(2, fi->buff_currentj_i[0]);    // current
      kernel_df.setArg(3, fi->buff_cji[0]);           // current
      kernel_df.setArg(4, sizeof(float), &par->a0_f); // scale factor
      queue.enqueueNDRangeKernel(kernel_df, cl::NullRange, cl::NDRange(n_cells), cl::NullRange);
      queue.finish();

      // read result arrays from the device to main memory

      //  cout << "\neions  " << timer.elapsed() << "s, \n";
      // sum total electron and ion densitiies and current densities for E B calculations
      kernel_dtotal.setArg(0, fi->buff_np_e[0]);       // np ion
      kernel_dtotal.setArg(1, fi->buff_np_i[0]);       // np ion
      kernel_dtotal.setArg(2, fi->buff_currentj_e[0]); // current
      kernel_dtotal.setArg(3, fi->buff_currentj_i[0]); // current
      kernel_dtotal.setArg(4, fi->buff_npt[0]);        // total particles density
      kernel_dtotal.setArg(5, fi->buff_jc[0]);         // total current density
      kernel_dtotal.setArg(6, sizeof(size_t), &n_cells);
      queue.enqueueNDRangeKernel(kernel_dtotal, cl::NullRange, cl::NDRange(n_cells / 16), cl::NullRange);
      queue.finish();

      // timer.mark();
      // set externally applied fields this is inside time loop so we can set time varying E and B field
      /*
      calcEeBe(Ee, Be, t); // find E field must work out every i,j,k depends on charge in every other cell
#ifdef Eon_
      resFFT = transferDataFromCPU(&vkGPU, fi->Ee, &fi->Ee_buffer, 3 * n_cells * sizeof(float));
#endif
#ifdef Bon_
      resFFT = transferDataFromCPU(&vkGPU, fi->Be, &fi->Be_buffer, 3 * n_cells * sizeof(float));
#endif
      */
      cdt = calcEBV(fi, par);
      // cout << "\nEBV: " << timer.elapsed() << "s, \n";
   }

   if (fastIO)
   { // is mapping required?
   }
   else
   {
      // for saving to disk
      queue.enqueueReadBuffer(pt->buff_x0_e[0], CL_TRUE, 0, n_partf, pt->pos0x[0]);
      queue.enqueueReadBuffer(pt->buff_y0_e[0], CL_TRUE, 0, n_partf, pt->pos0y[0]);
      queue.enqueueReadBuffer(pt->buff_z0_e[0], CL_TRUE, 0, n_partf, pt->pos0z[0]);
      queue.enqueueReadBuffer(pt->buff_x1_e[0], CL_TRUE, 0, n_partf, pt->pos1x[0]);
      queue.enqueueReadBuffer(pt->buff_y1_e[0], CL_TRUE, 0, n_partf, pt->pos1y[0]);
      queue.enqueueReadBuffer(pt->buff_z1_e[0], CL_TRUE, 0, n_partf, pt->pos1z[0]);

      queue.enqueueReadBuffer(pt->buff_x0_i[0], CL_TRUE, 0, n_partf, pt->pos0x[1]);
      queue.enqueueReadBuffer(pt->buff_y0_i[0], CL_TRUE, 0, n_partf, pt->pos0y[1]);
      queue.enqueueReadBuffer(pt->buff_z0_i[0], CL_TRUE, 0, n_partf, pt->pos0z[1]);
      queue.enqueueReadBuffer(pt->buff_x1_i[0], CL_TRUE, 0, n_partf, pt->pos1x[1]);
      queue.enqueueReadBuffer(pt->buff_y1_i[0], CL_TRUE, 0, n_partf, pt->pos1y[1]);
      queue.enqueueReadBuffer(pt->buff_z1_i[0], CL_TRUE, 0, n_partf, pt->pos1z[1]);

      queue.enqueueReadBuffer(pt->buff_q_e[0], CL_TRUE, 0, n_partf, pt->q[0]);
      queue.enqueueReadBuffer(pt->buff_q_i[0], CL_TRUE, 0, n_partf, pt->q[1]);

      queue.enqueueReadBuffer(pt->buff_q_e[0], CL_TRUE, 0, n_partf, pt->q[0]);
      queue.enqueueReadBuffer(pt->buff_q_i[0], CL_TRUE, 0, n_partf, pt->q[1]);

      queue.enqueueReadBuffer(fi->buff_E[0], CL_TRUE, 0, n_cellsf * 3, fi->E);
      queue.enqueueReadBuffer(fi->buff_B[0], CL_TRUE, 0, n_cellsf * 3, fi->B);

      queue.enqueueReadBuffer(fi->buff_np_e[0], CL_TRUE, 0, n_cellsf, fi->np[0]);
      queue.enqueueReadBuffer(fi->buff_np_i[0], CL_TRUE, 0, n_cellsf, fi->np[1]);

      queue.enqueueReadBuffer(fi->buff_currentj_e[0], CL_TRUE, 0, n_cellsf * 3, fi->currentj[0]);
      queue.enqueueReadBuffer(fi->buff_currentj_i[0], CL_TRUE, 0, n_cellsf * 3, fi->currentj[1]);
      if (changedt(pt, cdt, par))
      {
         queue.enqueueWriteBuffer(pt->buff_x0_e[0], CL_TRUE, 0, n_partf, pt->pos0x[0]);
         queue.enqueueWriteBuffer(pt->buff_y0_e[0], CL_TRUE, 0, n_partf, pt->pos0y[0]);
         queue.enqueueWriteBuffer(pt->buff_z0_e[0], CL_TRUE, 0, n_partf, pt->pos0z[0]);
         queue.enqueueWriteBuffer(pt->buff_x1_e[0], CL_TRUE, 0, n_partf, pt->pos1x[0]);
         queue.enqueueWriteBuffer(pt->buff_y1_e[0], CL_TRUE, 0, n_partf, pt->pos1y[0]);
         queue.enqueueWriteBuffer(pt->buff_z1_e[0], CL_TRUE, 0, n_partf, pt->pos1z[0]);

         queue.enqueueWriteBuffer(pt->buff_x0_i[0], CL_TRUE, 0, n_partf, pt->pos0x[1]);
         queue.enqueueWriteBuffer(pt->buff_y0_i[0], CL_TRUE, 0, n_partf, pt->pos0y[1]);
         queue.enqueueWriteBuffer(pt->buff_z0_i[0], CL_TRUE, 0, n_partf, pt->pos0z[1]);
         queue.enqueueWriteBuffer(pt->buff_x1_i[0], CL_TRUE, 0, n_partf, pt->pos1x[1]);
         queue.enqueueWriteBuffer(pt->buff_y1_i[0], CL_TRUE, 0, n_partf, pt->pos1y[1]);
         queue.enqueueWriteBuffer(pt->buff_z1_i[0], CL_TRUE, 0, n_partf, pt->pos1z[1]);
         // cout<<"change_dt done"<<endl;
      };
   }

   first = false;
}