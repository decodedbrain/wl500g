/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.

 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _STUBS_H
#define _STUBS_H 1

extern int dn_comp(const char *src, uint8_t *dst, int length, 
	uint8_t __attribute__((unused)) **dnptrs,
	uint8_t __attribute__((unused)) **lastdnptr);

extern int fflags(int sock, int flags);

#endif
