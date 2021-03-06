#ifndef _GEOAC_EIGENRAY_SPH_CPP_
#define _GEOAC_EIGENRAY_SPH_CPP_

#include <math.h>
#include <iostream>
#include <iomanip>
#include <fstream>

#include "geoac.params.h"
#include "geoac.eqset.h"
#include "geoac.interface.h"
#include "geoac.eigenray.h"

#include "../atmo/atmo_state.h"

#include "../util/rk4solver.h"
#include "../util/globe.h"

// NOTE: All angles are assumed to be radians (convert deg->rad in {}_main.cpp file)
// Also, the source array contains (altitude, latitude, longitude) while the reciever
// array contains (latitude, longitude).  Latitudes and longitudes are converted to
// radians in the main.cpp file and altitude is relative to the wgs84 ellipsoid.

using namespace std;

bool geoac::verbose = false;
int geoac::eigenray_cnt = 0;

double geoac::damping = 1.0e-3;
double geoac::tolerance = 0.1;

double geoac::dth_big = 0.1 * (Pi / 180.0);
double geoac::dth_sml = 0.001 * (Pi / 180.0);

ofstream geoac::eig_results;

double geoac::mod_dth(double dr, double dr_dtheta){ return dth_big - (dth_big - dth_sml) * exp(- 1.0 / 2.0 * pow(dr / dr_dtheta, 2));}


bool geoac::est_eigenray(double src[3], double rcvr[2], double th_min, double th_max, double & th_est, double & ph_est, double & th_next, int bncs, double az_err_lim){
    double rcvr_rng = globe::gc_dist(src[1], src[2], rcvr[0], rcvr[1]);
    double rcvr_az = globe::bearing(src[1], src[2], rcvr[0], rcvr[1]);
    
    if(verbose){
        cout << '\t' << "Estimating eigenray angles for source-receiver at great circle distance " << rcvr_rng << " km and azimuth " << rcvr_az * (180.0 / Pi) << " degrees.  Inclination limits: [" << th_min * (180.0 / Pi) << ", " << th_max * (180.0 / Pi) << "]." << '\n';
    }
    
    int	iterations = 0, k, length = s_max * int(1.0 / (ds_min * 10));
    double r, r_prev, dth = dth_big, dph = 100.0;
    bool break_check, success, th_max_reached;

    calc_amp = false;
    configure();
    
    double** solution;
    build_solution(solution, length);
    
    phi = Pi / 2.0 - rcvr_az;
    success = 0;
    th_max_reached = false;
    while(fabs(dph) > az_err_lim && iterations < 5){
        r = rcvr_rng;
        r_prev = rcvr_rng;
        
        for(double th = th_min; th < th_max; th += dth){
            if(th + dth >= th_max) th_max_reached = true;
            
            theta = th;
            set_initial(solution, src[0], src[1], src[2]);
            
            k = prop_rk4(solution, break_check);
            if(!break_check){
                for(int n_bnc = 1; n_bnc <= bncs; n_bnc++){
                    set_refl(solution,k);
                    k = prop_rk4(solution, break_check);
                    if(break_check) break;
                }
            }
            
            if(break_check){ r = rcvr_rng; r_prev = rcvr_rng; success = false; }
            else {           r = globe::gc_dist(src[1], src[2], solution[k][1], solution[k][2]);}
            
            if(verbose){
                cout << '\t' << '\t' << "Ray launched with inclination " << theta * (180.0 / Pi) << " degrees arrives at range " << r;
                cout << " km after " << bncs << " bounces.  Exact arrival at " << solution[k][1] * (180.0 / Pi) << " degrees N latitude, " << solution[k][2] * (180.0 / Pi) << " degrees E longitude" << '\n';
            }
            
            if((r - rcvr_rng) * (r_prev - rcvr_rng) < 0.0){
                if(iterations==0) th_next = theta;
                
                dph  = globe::bearing(src[1], src[2], rcvr[0], rcvr[1]);
                dph -= globe::bearing(src[1], src[2], solution[k][1], solution[k][2]);
                while(dph >  Pi){ dph -= 2.0 * Pi;}
                while(dph < -Pi){ dph += 2.0 * Pi;}
                
                if(fabs(dph) < az_err_lim){
                    if(verbose) cout << '\t' << '\t' << "Azimuth deviation less than " << az_err_lim << " degrees.  Estimates acceptable." << '\n' << '\n';
                    th_est = theta - dth; ph_est = phi;
                    delete_solution(solution, length);
                    return true;
                } else {
                    if(verbose) cout << '\t' << '\t' << "Azimuth deviation greater than " << az_err_lim << " degrees.  Compensating and searching inclinations again." << '\n' << '\n';
                    phi += dph * 0.9; th_min = max(theta - 10.0 * (Pi / 180.0), th_min);
                }
                break;
            }
            if(iterations >= 3){ dth = mod_dth(r - rcvr_rng, (r - r_prev)/(2.0 * dth));}
            r_prev = r;
        }
        if(th_max_reached){ th_next = th_max; break;}
        iterations++;
    }
    delete_solution(solution, length);
    
    if(verbose) cout << '\t' << '\t' << "Reached maximum inclination angle or iteration limit." << '\n' << '\n';
    return false;
}


void geoac::find_eigenray(double src[3], double rcvr[2], double th_est, double ph_est, double freq, int bnc_cnt, int iterate_limit, char title[]){
	bool break_check;
    char output_buffer [512];
    ofstream raypath;
    double D, attenuation, travel_time_sum, r_max, inclination, back_az, back_az_dev, dr, dr_prev = 10000.0, step_sc = 1.0;
    long double lat, lon, r_grnd, c_src, c_grnd, dzg_dlat, dzg_dlon, ds_norm, ds_dth, ds_dph, dlat, dlon, dlat_dth, dlon_dth, dlat_dph, dlon_dph;
    long double det, dth, dph;
    
    int	k, length = s_max * int(1.0 / (ds_min * 10));

    calc_amp = true;
    configure();

    double** solution;
    build_solution(solution, length);

    theta =	th_est;
    phi = 	ph_est;
    
    if(verbose) cout << '\t' << '\t' << "Searching for exact eigenray using auxiliary parameters." << '\n';
	for(int n = 0; n <= iterate_limit; n++){
        if(n == iterate_limit){ if(verbose){ cout << '\t' <<'\t' << '\t' << "Search for exact eigenray maxed out iterations.  No eigneray idenfied." << '\n';} break;}
        
		set_initial(solution, src[0], src[1], src[2]);
        if(verbose) cout << '\t' << '\t' << "Calculating ray path: " << theta * (180.0 / Pi) << " degrees inclination, " << 90.0 - phi * (180.0 / Pi) << " degrees azimuth";
		
        k = prop_rk4(solution, break_check);
        if(break_check){ if(verbose){ cout << '\t' << "Ray path left propagation region." << '\n';} break;}
        for(int n_bnc = 1; n_bnc <= bnc_cnt; n_bnc++){
            set_refl(solution, k); k = prop_rk4(solution, break_check);
            if(break_check){ if(verbose){ cout << '\t' << "Ray path left propagation region." << '\n';} break;}
        }
        if(break_check) break;
        
		// Determine arrival location and check if it's within the defined tolerance
        dr = globe::gc_dist(solution[k][1], solution[k][2], rcvr[0], rcvr[1]);
        if(verbose) cout << '\t' << '\t' << "Arrival at (" << setprecision(8) << solution[k][1] * (180.0 / Pi) << ", " << solution[k][2] * (180.0 / Pi) << "), distance to receiver = " << dr << " km." << '\n';

        if(dr < tolerance) {
            sprintf(output_buffer, "%s.eigenray-%i.dat", title, eigenray_cnt);
            raypath.open(output_buffer);

            raypath << "# lat [deg]";
            raypath << '\t' << "lon [deg]";
            raypath << '\t' << "z [km]";
            raypath << '\t' << "geo. atten. [dB]";
            raypath << '\t' << "absorption [dB]";
            raypath << '\t' << "time [s]";
            raypath << '\n';
                
            attenuation = 0.0;
            travel_time_sum = 0.0;
            r_max = 0.0;
            
            set_initial(solution, src[0], src[1], src[2]);
            k = prop_rk4(solution, break_check);

            for(int m = 1; m < k; m++){
                travel_time(travel_time_sum, solution, m - 1, m);
                atten(attenuation, solution, m - 1, m, freq);
                r_max = max (r_max, solution[m][0] - globe::r0);
                
                if(m == 1 || m % 15 == 0){
                    raypath << setprecision(8) << solution[m][1] * (180.0 / Pi);
                    raypath << '\t' << setprecision(8) << solution[m][2] * (180.0 / Pi);
                    raypath << '\t' << solution[m][0] - globe::r0;
                    raypath << '\t' << 10.0 * log10(amp(solution, m));
                    raypath << '\t' << -attenuation;
                    raypath << '\t' << travel_time_sum << '\n';
                }
            }
            for(int n_bnc = 1; n_bnc <= bnc_cnt; n_bnc++){
                set_refl(solution, k);
                
                k = prop_rk4(solution, break_check);
                for(int m = 1; m < k; m++){
                    travel_time(travel_time_sum, solution, m - 1, m);
                    atten(attenuation, solution, m - 1, m, freq);
                    r_max = max (r_max, solution[m][0] - globe::r0);
                    
                    if(m == 1 || m % 15 == 0){
                        raypath << setprecision(8) << solution[m][1] * (180.0 / Pi);
                        raypath << '\t' << setprecision(8) << solution[m][2] * (180.0 / Pi);
                        raypath << '\t' << solution[m][0] - globe::r0;
                        raypath << '\t' << 10.0 * log10(amp(solution, m));
                        raypath << '\t' << -attenuation;
                        raypath << '\t' << travel_time_sum << '\n';
                    }
                }
            }
            raypath.close();
                        
            inclination = - asin(atmo::c(solution[k][0], solution[k][1], solution[k][2]) / atmo::c(src[0], src[1], src[2]) * solution[k][3]) * 180.0 / Pi;
            back_az = 90.0 - atan2(-solution[k][4], -solution[k][5]) * 180.0 / Pi;
            while(back_az < -180.0) back_az += 360.0;
            while(back_az >  180.0) back_az -= 360.0;
            
            back_az_dev = ((Pi / 2.0 - atan2(-solution[k][4], -solution[k][5]) ) - globe::bearing(rcvr[0], rcvr[1], src[1], src[2]) ) * (180.0 / Pi);
            if(back_az_dev >  180.0) back_az_dev -= 360.0;
            if(back_az_dev < -180.0) back_az_dev += 360.0;
                
            
            if(!verbose){cout << '\t' << "Eigenray identified:" << '\t' << "theta, phi = " << setprecision(8) << theta * (180.0 / Pi) << ", " << 90.0 - phi * (180.0 / Pi) << " degrees." << '\n';
            } else {
                cout << '\t' << '\t' << "Eigenray-" << eigenray_cnt << ":" << '\n';
                cout << '\t' << '\t' << '\t' << "inclination [deg] = " << theta * (180.0 / Pi) << '\n';
                cout << '\t' << '\t' << '\t' << "azimuth [deg] = " << 90.0 - phi * (180.0 / Pi) << '\n';
                cout << '\t' << '\t' << '\t' << "bounces [-] = " << bnc_cnt << '\n';
                cout << '\t' << '\t' << '\t' << "latitude [deg] = " << setprecision(8) << solution[k][1] * 180.0 / Pi << '\n';
                cout << '\t' << '\t' << '\t' << "longitude [deg] = " << setprecision(8) << solution[k][2] * 180.0 / Pi << '\n';
                cout << '\t' << '\t' << '\t' << "time [s] = " << travel_time_sum << '\n';
                cout << '\t' << '\t' << '\t' << "celerity [km/s] = " << globe::gc_dist(solution[k][1], solution[k][2], src[1], src[2]) / travel_time_sum << '\n';
                cout << '\t' << '\t' << '\t' << "turning height [km] = " << r_max << '\n';
                cout << '\t' << '\t' << '\t' << "arrival inclination [deg] = " << inclination << '\n';
                cout << '\t' << '\t' << '\t' << "back azimuth [deg] = " << back_az << '\n';
                cout << '\t' << '\t' << '\t' << "attenuation (geometric) [dB] = " << 10.0 * log10(geoac::amp(solution,k)) << '\n';
                cout << '\t' << '\t' << '\t' << "absorption [dB] = " << -attenuation << '\n' << '\n';
            }
                       
            eig_results << setprecision(8) << theta * (180.0 / Pi);
            eig_results << '\t' << setprecision(8) << 90.0 - phi * (180.0 / Pi);
            eig_results << '\t' << bnc_cnt;
            eig_results << '\t' << setprecision(8) << solution[k][1] * 180.0 / Pi;
            eig_results << '\t' << setprecision(8) << solution[k][2] * 180.0 / Pi;
            eig_results << '\t' << travel_time_sum;
            eig_results << '\t' << globe::gc_dist(solution[k][1], solution[k][2], src[1], src[2]) / travel_time_sum;
            eig_results << '\t' << r_max;
            eig_results << '\t' << inclination;
            eig_results << '\t' << back_az;
            eig_results << '\t' << 10.0 * log10(geoac::amp(solution,k));
            eig_results << '\t' << -attenuation;
            eig_results << '\n';
            
            eigenray_cnt++;
            
            break;
        } else if (n > 0 && dr > dr_prev){
            theta -= dth * step_sc;
            phi -= dph * step_sc;
            step_sc /= 2.0;
            if(sqrt(dth * dth + dph * dph) * step_sc < 1.0e-12){
                if (verbose) cout << '\t' << '\t' <<  '\t' << "Step size too small, psuedo-critical ray path likely." << '\n' << '\n';
                break;
            }
        } else {
            step_sc = min(1.0, step_sc * 1.25);
            
            dlat = rcvr[0] - solution[k][1];
            dlon = rcvr[1] - solution[k][2];
            
            r_grnd = topo::z(solution[k][1], solution[k][2]);
            c_grnd = atmo::c(r_grnd, solution[k][1], solution[k][2]);
            c_src = atmo::c(src[0] + globe::r0, src[1], src[2]);
            
            dzg_dlat = topo::dz(solution[k][1], solution[k][2], 0) / r_grnd;
            dzg_dlon = topo::dz(solution[k][1], solution[k][2], 1) / (r_grnd * cos(solution[k][1]));
            ds_norm = solution[k][3] - dzg_dlat * solution[k][4] -  dzg_dlon  * solution[k][5];
            
            ds_dth = - c_src / c_grnd * (solution[k][6] -  dzg_dlat * r_grnd * solution[k][7] -  dzg_dlon * (r_grnd * cos(solution[k][1])) * solution[k][8])  / ds_norm;
            ds_dph = - c_src / c_grnd * (solution[k][12] - dzg_dlat * r_grnd * solution[k][13] - dzg_dlon * (r_grnd * cos(solution[k][1])) * solution[k][14]) / ds_norm;

            dlat_dth = solution[k][7]  + solution[k][4] / r_grnd * ds_dth;  dlon_dth = solution[k][8]  + solution[k][5] / (r_grnd * cos(solution[k][1])) * ds_dth;
            dlat_dph = solution[k][13] + solution[k][4] / r_grnd * ds_dph;  dlon_dph = solution[k][14] + solution[k][5] / (r_grnd * cos(solution[k][1])) * ds_dph;
            
            det = pow(1.0 + damping, 2) * dlat_dth * dlon_dph - dlat_dph * dlon_dth;
            dth = ((1.0 + damping) * dlon_dph * dlat - dlat_dph * dlon) / det;
            dph = ((1.0 + damping) * dlat_dth * dlon - dlon_dth * dlat) / det;
            
            theta += dth * step_sc;
            phi += dph * step_sc;
            
            dr_prev = dr;
        }
        clear_solution(solution, k);
	}
    delete_solution(solution, length);
}
#endif /* _GEOAC_EIGENRAY_SPH_CPP_ */
